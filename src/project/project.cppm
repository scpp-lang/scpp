module;

#include <unistd.h>
#include <cstdlib>

export module scpp.project;

import std;
import scpp.ast;
import scpp.lexer;
import scpp.driver;

export namespace scpp {

class ProjectBuildOptions {
public:
    virtual ~ProjectBuildOptions() = default;
    bool build_lib_only = false;
    std::optional<std::string> selected_bin{};
    std::optional<std::string> selected_lib{};
    std::optional<std::string> selected_package{};
    bool build_workspace = false;
};

std::optional<std::string> find_project_manifest(const std::string& start_dir);
int build_manifest_project(const std::string& start_dir, const ProjectBuildOptions& options);

} // namespace scpp

namespace scpp {

extern "C" {
    struct sqlite3;
    struct sqlite3_stmt;
    int sqlite3_open(const char* filename, sqlite3** ppDb);
    int sqlite3_close(sqlite3* db);
    const char* sqlite3_errmsg(sqlite3* db);
    int sqlite3_exec(sqlite3* db, const char* sql, void* callback, void* arg, char** errmsg);
    void sqlite3_free(void* ptr);
    int sqlite3_prepare_v2(sqlite3* db, const char* zSql, int nByte, sqlite3_stmt** ppStmt, const char** pzTail);
    int sqlite3_step(sqlite3_stmt* stmt);
    int sqlite3_finalize(sqlite3_stmt* stmt);
    int sqlite3_bind_text(sqlite3_stmt* stmt, int index, const char* val, int len, void* destructor);
    const char* sqlite3_column_text(sqlite3_stmt* stmt, int iCol);

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
    int lstat(const char* pathname, posix_stat* statbuf);
    int rmdir(const char* pathname);
    int mkdir(const char* pathname, unsigned int mode);
    int symlink(const char* target, const char* linkpath);
    char* getcwd(char* buf, unsigned long size);
    int open(const char* pathname, int flags, ...);
    int close(int fd);
    long read(int fd, void* buf, unsigned long count);
    long write(int fd, const void* buf, unsigned long count);
    int system(const char* command);
#ifndef __clang__
    int access(const char* pathname, int mode);
    int unlink(const char* pathname);
    long readlink(const char* path, char* buf, unsigned long bufsiz);
    char* realpath(const char* path, char* resolved_path);
    char* getenv(const char* name);
#endif
}

constexpr int SQLITE_OK = 0;
constexpr int SQLITE_ROW = 100;
constexpr int SQLITE_DONE = 101;

inline void eprint(std::string_view msg) {
    if (msg.empty()) return;
    [[scpp::unsafe]] {
        write(2, msg.data(), static_cast<unsigned long>(msg.size()));
    }
}

inline void eprintln(std::string_view msg) {
    eprint(msg);
    eprint("\n");
}

class ManifestError {
public:
    virtual ~ManifestError() = default;
    ManifestError() = default;
    ManifestError(const ManifestError& other) : message{other.message} {}
    void operator=(const ManifestError& other) { this->message = other.message; }
    ManifestError(std::string msg) : message{std::move(msg)} {}
    std::string message{};
    const std::string& what() const { return this->message; }
};

class BuildError {
public:
    virtual ~BuildError() = default;
    BuildError() = default;
    BuildError(const BuildError& other) : message{other.message} {}
    void operator=(const BuildError& other) { this->message = other.message; }
    BuildError(std::string msg) : message{std::move(msg)} {}
    std::string message{};
    const std::string& what() const { return this->message; }
};

bool trace_enabled() {
    const char* env = nullptr;
    char first = '\0';
    [[scpp::unsafe]] {
        env = getenv("SCPP_BUILD_TRACE");
        if (env != nullptr) first = env[0];
    }
    return env != nullptr && first != '\0' && std::string_view{env} != "0";
}

void trace_build(const std::string& message) {
    if (!trace_enabled()) return;
    eprintln("[scpp-build] " + message);
}

inline std::string string_from_view(std::string_view sv) {
    return std::string{sv.data(), sv.size()};
}

inline bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && is_ascii_space(text.at(start))) start++;
    std::size_t end = text.size();
    while (end > start && is_ascii_space(text.at(end - 1))) end--;
    return string_from_view(text.substr(start, end - start));
}

inline std::string path_parent(std::string_view p) {
    if (p.empty()) return std::string{"."};
    std::size_t slash = p.rfind("/");
    if (slash == std::string_view::npos) return std::string{"."};
    if (slash == 0) return std::string{"/"};
    return string_from_view(p.substr(0, slash));
}

inline std::string path_filename(std::string_view p) {
    if (p.empty()) return std::string{};
    std::size_t slash = p.rfind("/");
    if (slash == std::string_view::npos) return string_from_view(p);
    return string_from_view(p.substr(slash + 1));
}

inline std::string path_join(std::string_view a, std::string_view b) {
    if (a.empty()) return string_from_view(b);
    if (b.empty()) return string_from_view(a);
    if (a.at(a.size() - 1) == '/') return string_from_view(a) + string_from_view(b);
    return string_from_view(a) + "/" + string_from_view(b);
}

inline bool path_exists(const std::string& path) {
    int rc = -1;
    [[scpp::unsafe]] {
        rc = access(path.c_str(), 0);
    }
    return rc == 0;
}

inline bool path_is_directory(const std::string& path) {
    posix_stat st{};
    int rc = -1;
    [[scpp::unsafe]] {
        rc = stat(path.c_str(), &st);
    }
    return rc == 0 && (st.st_mode & 0170000) == 0040000;
}

inline bool path_is_regular_file(const std::string& path) {
    posix_stat st{};
    int rc = -1;
    [[scpp::unsafe]] {
        rc = stat(path.c_str(), &st);
    }
    return rc == 0 && (st.st_mode & 0170000) == 0100000;
}

inline bool path_is_symlink(const std::string& path) {
    posix_stat st{};
    int rc = -1;
    [[scpp::unsafe]] {
        rc = lstat(path.c_str(), &st);
    }
    return rc == 0 && (st.st_mode & 0170000) == 0120000;
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

inline std::string path_lexically_normal(std::string_view p) {
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
            if (!segments.empty() && segments[segments.size() - 1] != "..") {
                segments.pop_back();
            } else if (!is_absolute) {
                segments.push_back(std::string{".."});
            }
        } else {
            segments.push_back(string_from_view(seg));
        }
    }
    if (segments.empty()) {
        return is_absolute ? std::string{"/"} : std::string{"."};
    }
    std::string result{};
    for (std::size_t idx = 0; idx < segments.size(); idx++) {
        if (is_absolute || idx > 0) result += "/";
        result += segments[idx];
    }
    return result;
}

inline std::string normalized_path(const std::string& path) {
    if (path.empty()) return path_current();
    char buf[4096] = {};
    char* res = nullptr;
    [[scpp::unsafe]] {
        res = realpath(path.c_str(), buf);
    }
    if (res != nullptr) {
        return std::string{buf};
    }
    std::string parent = path_parent(path);
    std::string filename = path_filename(path);
    if (parent != "." && parent != path) {
        std::string norm_parent = normalized_path(parent);
        return path_join(norm_parent, filename);
    }
    std::string abs_path = (!path.empty() && path.at(0) == '/') ? path : path_join(path_current(), path);
    return path_lexically_normal(abs_path);
}

inline bool path_create_directories(const std::string& path) {
    if (path.empty() || path == "." || path == "/") return true;
    if (path_exists(path)) return true;
    std::string parent = path_parent(path);
    if (!parent.empty() && parent != path && parent != ".") {
        if (!path_create_directories(parent)) return false;
    }
    int rc = -1;
    [[scpp::unsafe]] {
        rc = mkdir(path.c_str(), 0755);
    }
    return rc == 0 || path_exists(path);
}

inline bool path_remove_all(const std::string& path) {
    if (!path_exists(path)) {
        int rc = -1;
        [[scpp::unsafe]] {
            rc = unlink(path.c_str());
        }
        return rc == 0;
    }
    if (path_is_directory(path) && !path_is_symlink(path)) {
        void* dir = nullptr;
        [[scpp::unsafe]] {
            dir = opendir(path.c_str());
        }
        if (dir != nullptr) {
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
                path_remove_all(path_join(path, name));
            }
            [[scpp::unsafe]] {
                closedir(dir);
            }
        }
        int rc = -1;
        [[scpp::unsafe]] {
            rc = rmdir(path.c_str());
        }
        return rc == 0;
    }
    int rc = -1;
    [[scpp::unsafe]] {
        rc = unlink(path.c_str());
    }
    return rc == 0;
}

inline bool path_symlink(const std::string& target, const std::string& linkpath) {
    int rc = -1;
    [[scpp::unsafe]] {
        rc = symlink(target.c_str(), linkpath.c_str());
    }
    return rc == 0;
}

template<typename T>
void sort_vector(std::vector<T>& vec) {
    if (vec.size() <= 1) return;
    for (std::size_t i = 1; i < vec.size(); i++) {
        T key = vec[i];
        std::size_t j = i;
        while (j > 0 && vec[j - 1] > key) {
            vec[j] = vec[j - 1];
            j--;
        }
        vec[j] = key;
    }
}

template<typename T, typename Compare>
void sort_vector_by(std::vector<T>& vec, Compare comp) {
    if (vec.size() <= 1) return;
    for (std::size_t i = 1; i < vec.size(); i++) {
        T key = vec[i];
        std::size_t j = i;
        while (j > 0 && comp(key, vec[j - 1])) {
            vec[j] = vec[j - 1];
            j--;
        }
        vec[j] = key;
    }
}

template<typename T>
bool vector_contains(const std::vector<T>& vec, const T& item) {
    for (std::size_t i = 0; i < vec.size(); i++) {
        if (vec[i] == item) return true;
    }
    return false;
}

void append_unique_strings(std::vector<std::string>& into, const std::vector<std::string>& extra) {
    for (std::size_t i = 0; i < extra.size(); i++) {
        if (!vector_contains(into, extra[i])) into.push_back(extra[i]);
    }
}

void append_unique_paths(std::vector<std::string>& into, const std::vector<std::string>& extra) {
    for (std::size_t i = 0; i < extra.size(); i++) {
        if (!vector_contains(into, extra[i])) into.push_back(extra[i]);
    }
}

struct SetInsertResult {
    bool second = false;
};

class StringSet {
public:
    virtual ~StringSet() = default;
    std::vector<std::string> elements_{};

    bool contains(const std::string& item) const {
        return vector_contains(this->elements_, item);
    }

    SetInsertResult insert(const std::string& item) {
        if (this->contains(item)) return SetInsertResult{false};
        this->elements_.push_back(item);
        return SetInsertResult{true};
    }

    void erase(const std::string& item) {
        for (std::size_t i = 0; i < this->elements_.size(); i++) {
            if (this->elements_[i] == item) {
                for (std::size_t j = i; j + 1 < this->elements_.size(); j++) {
                    this->elements_[j] = this->elements_[j + 1];
                }
                this->elements_.pop_back();
                return;
            }
        }
    }
};

template<typename V>
class StringMap {
public:
    virtual ~StringMap() = default;
    StringMap() = default;
    StringMap(const StringMap& other) {
        for (std::size_t i = 0; i < other.keys_.size(); i++) {
            this->keys_.push_back(other.keys_[i]);
            this->values_.push_back(other.values_[i]);
        }
    }
    void operator=(const StringMap& other) {
        this->keys_.clear();
        this->values_.clear();
        for (std::size_t i = 0; i < other.keys_.size(); i++) {
            this->keys_.push_back(other.keys_[i]);
            this->values_.push_back(other.values_[i]);
        }
    }

    std::vector<std::string> keys_{};
    std::vector<V> values_{};

    std::size_t size() const { return this->keys_.size(); }
    bool empty() const { return this->keys_.empty(); }

    bool contains(const std::string& key) const {
        for (std::size_t i = 0; i < this->keys_.size(); i++) {
            if (this->keys_[i] == key) return true;
        }
        return false;
    }

    const V& at(const std::string& key) const {
        for (std::size_t i = 0; i < this->keys_.size(); i++) {
            if (this->keys_[i] == key) return this->values_[i];
        }
        std::abort();
        return this->values_[0];
    }

    V& at(const std::string& key) {
        for (std::size_t i = 0; i < this->keys_.size(); i++) {
            if (this->keys_[i] == key) return this->values_[i];
        }
        std::abort();
        return this->values_[0];
    }

    void emplace(const std::string& key, V val) {
        for (std::size_t i = 0; i < this->keys_.size(); i++) {
            if (this->keys_[i] == key) {
                this->values_[i] = std::move(val);
                return;
            }
        }
        this->keys_.push_back(key);
        this->values_.push_back(std::move(val));
    }

    void insert_or_assign(const std::string& key, V val) {
        this->emplace(key, std::move(val));
    }
};

inline std::unordered_map<std::string, std::string> to_std_map(const StringMap<std::string>& sm) {
    std::unordered_map<std::string, std::string> res{};
    for (std::size_t i = 0; i < sm.size(); i++) {
        res.emplace(sm.keys_[i], sm.values_[i]);
    }
    return res;
}

std::string quote_for_shell(const std::string& text) {
    std::string quoted{"\""};
    for (std::size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::string escape_json(std::string_view text) {
    std::string out{};
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); i++) {
        char ch = text.at(i);
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
    std::string out{};
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); i++) {
        char ch = raw.at(i);
        if (is_ascii_alnum(ch)) {
            out.push_back(ch);
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) out = std::string{"target"};
    return out;
}

[[nodiscard]] inline std::expected<std::string, BuildError> read_file(const std::string& path) {
    int fd = -1;
    [[scpp::unsafe]] {
        fd = open(path.c_str(), 0);
    }
    if (fd < 0) {
        return std::unexpected(BuildError{"cannot open file '" + path + "'"});
    }
    std::string result{};
    char buf[4096] = {};
    while (true) {
        long n = 0;
        [[scpp::unsafe]] {
            n = read(fd, &buf[0], 4096);
        }
        if (n < 0) {
            [[scpp::unsafe]] { close(fd); }
            return std::unexpected(BuildError{"failed reading file: " + path});
        }
        if (n == 0) break;
        for (long i = 0; i < n; i++) {
            result.push_back(buf[i]);
        }
    }
    [[scpp::unsafe]] {
        close(fd);
    }
    return result;
}

[[nodiscard]] inline std::expected<void, BuildError> write_file(const std::string& path, std::string_view content) {
    int fd = -1;
    [[scpp::unsafe]] {
        fd = open(path.c_str(), 0x241, 0644);
    }
    if (fd < 0) {
        return std::unexpected(BuildError{"cannot write file '" + path + "'"});
    }
    std::size_t total = 0;
    while (total < content.size()) {
        long n = 0;
        [[scpp::unsafe]] {
            n = write(fd, content.data() + total, static_cast<unsigned long>(content.size() - total));
        }
        if (n <= 0) {
            [[scpp::unsafe]] { close(fd); }
            return std::unexpected(BuildError{"cannot write file '" + path + "'"});
        }
        total += static_cast<std::size_t>(n);
    }
    [[scpp::unsafe]] {
        close(fd);
    }
    return {};
}

std::string to_hex_u64(unsigned long value) {
    const char hex_chars[17] = "0123456789abcdef";
    char buf[17] = {};
    for (std::size_t i = 0; i < 16; i++) {
        buf[15 - i] = hex_chars[value & 0x0f];
        value >>= 4;
    }
    buf[16] = '\0';
    return std::string{buf};
}

std::string fnv1a64_hex(std::string_view text) {
    constexpr unsigned long offset_basis = 14695981039346656037ull;
    constexpr unsigned long prime = 1099511628211ull;
    unsigned long value = offset_basis;
    for (std::size_t i = 0; i < text.size(); i++) {
        std::uint8_t ch = static_cast<std::uint8_t>(text.at(i));
        value ^= static_cast<unsigned long>(ch);
        value *= prime;
    }
    return to_hex_u64(value);
}

std::string fnv1a64_hex(const std::string& text) {
    return fnv1a64_hex(std::string_view{text});
}

std::string digest_file(const std::string& path) {
    auto content = read_file(path);
    if (!content.has_value()) return std::string{};
    return fnv1a64_hex(std::string_view{content.value()});
}

std::string join_for_digest(const std::vector<std::string>& values) {
    std::string out{};
    for (std::size_t i = 0; i < values.size(); i++) {
        out += std::to_string(values[i].size()) + ":" + values[i] + ";";
    }
    return out;
}

std::string path_digest_or_empty(const std::string& path) {
    return path_exists(path) ? digest_file(path) : std::string{};
}

std::vector<std::string> path_digests(const std::vector<std::string>& paths) {
    std::vector<std::string> digests{};
    digests.reserve(paths.size());
    for (std::size_t i = 0; i < paths.size(); i++) {
        digests.push_back(paths[i] + "#" + path_digest_or_empty(paths[i]));
    }
    return digests;
}

bool glob_match(std::string_view pattern, std::string_view path) {
    if (pattern.empty()) return path.empty();
    if (pattern.size() >= 2 && pattern.at(0) == '*' && pattern.at(1) == '*') {
        std::string_view rest = pattern.substr(2);
        if (rest.empty()) return true;
        if (rest.at(0) == '/') {
            rest = rest.substr(1);
            if (glob_match(rest, path)) return true;
            for (std::size_t i = 0; i < path.size(); i++) {
                if (path.at(i) == '/') {
                    if (glob_match(rest, path.substr(i + 1))) return true;
                }
            }
            return false;
        }
        for (std::size_t i = 0; i <= path.size(); i++) {
            if (glob_match(rest, path.substr(i))) return true;
        }
        return false;
    }
    if (pattern.at(0) == '*') {
        std::string_view rest = pattern.substr(1);
        if (glob_match(rest, path)) return true;
        for (std::size_t i = 0; i < path.size() && path.at(i) != '/'; i++) {
            if (glob_match(rest, path.substr(i + 1))) return true;
        }
        return false;
    }
    if (pattern.at(0) == '?') {
        if (!path.empty() && path.at(0) != '/') return glob_match(pattern.substr(1), path.substr(1));
        return false;
    }
    if (!path.empty() && pattern.at(0) == path.at(0)) return glob_match(pattern.substr(1), path.substr(1));
    return false;
}

void collect_files_recursive(const std::string& dir, const std::string& rel_prefix,
                            std::vector<std::pair<std::string, std::string>>& out) {
    void* d = nullptr;
    [[scpp::unsafe]] {
        d = opendir(dir.c_str());
    }
    if (d == nullptr) return;
    while (true) {
        dirent* entry = nullptr;
        [[scpp::unsafe]] {
            entry = readdir(d);
        }
        if (entry == nullptr) break;
        std::string name{};
        [[scpp::unsafe]] {
            name = std::string{entry->d_name};
        }
        if (name == "." || name == "..") continue;
        std::string full_path = path_join(dir, name);
        std::string rel_path = rel_prefix.empty() ? name : path_join(rel_prefix, name);
        if (path_is_directory(full_path)) {
            collect_files_recursive(full_path, rel_path, out);
        } else if (path_is_regular_file(full_path)) {
            std::pair<std::string, std::string> p{full_path, rel_path};
            out.push_back(std::move(p));
        }
    }
    [[scpp::unsafe]] {
        closedir(d);
    }
}

std::vector<std::string> expand_source_patterns(const std::string& base_dir,
                                               const std::vector<std::string>& patterns) {
    std::vector<std::pair<std::string, std::string>> files{};
    collect_files_recursive(base_dir, "", files);
    std::vector<std::string> results{};
    StringSet seen{};
    for (std::size_t i = 0; i < files.size(); i++) {
        const std::string& full_path = files[i].first;
        const std::string& rel_path = files[i].second;
        for (std::size_t j = 0; j < patterns.size(); j++) {
            if (glob_match(patterns[j], rel_path)) {
                std::string norm = normalized_path(full_path);
                if (seen.insert(norm).second) {
                    results.push_back(norm);
                }
                break;
            }
        }
    }
    sort_vector(results);
    return results;
}

std::string strip_toml_comment(std::string_view line) {
    bool in_string = false;
    bool escape = false;
    std::string out{};
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); i++) {
        char ch = line.at(i);
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
        std::string inner = string_from_view(body);
        if (inner.starts_with("\r\n")) {
            inner = inner.substr(2);
        } else if (!inner.empty() && inner.at(0) == '\n') {
            inner = inner.substr(1);
        }

        std::string out{};
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
                    return std::unexpected(ManifestError{context + " ends with an incomplete escape sequence"});
                }
                i++;
                switch (inner[i]) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    default: return std::unexpected(ManifestError{context + " contains an unsupported escape sequence"});
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
    if (text.size() < 2 || text.at(0) != '"' || text.at(text.size() - 1) != '"') {
        return std::unexpected(ManifestError{context + " must be a TOML string"});
    }
    std::string out{};
    out.reserve(text.size() - 2);
    bool escape = false;
    for (std::size_t i = 1; i + 1 < text.size(); i++) {
        char ch = text.at(i);
        if (escape) {
            switch (ch) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                default: return std::unexpected(ManifestError{context + " contains an unsupported escape sequence"});
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
    if (escape) return std::unexpected(ManifestError{context + " ends with an incomplete escape sequence"});
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
    for (std::size_t i = 0; i < text.size(); i++) {
        char ch = text.at(i);
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

struct LineReader {
    std::string_view content{};
    std::size_t pos{};
    std::int64_t line_number = 1;

    bool next_line(std::string_view& out) {
        if (this->pos > this->content.size()) return false;
        if (this->pos == this->content.size()) {
            this->pos++;
            return false;
        }
        std::size_t start = this->pos;
        std::size_t nl = std::string_view::npos;
        for (std::size_t i = this->pos; i < this->content.size(); i++) {
            if (this->content.at(i) == '\n') {
                nl = i;
                break;
            }
        }
        if (nl == std::string_view::npos) {
            this->pos = this->content.size() + 1;
            out = this->content.substr(start);
        } else {
            this->pos = nl + 1;
            std::size_t len = nl - start;
            if (len > 0 && this->content.at(start + len - 1) == '\r') len--;
            out = this->content.substr(start, len);
        }
        this->line_number++;
        return true;
    }
};

[[nodiscard]] std::expected<std::string, ManifestError> read_manifest_value(
    std::string value, LineReader& reader, const std::string& manifest_path) {
    value = trim(value);
    bool needs_multiline_string = starts_multiline_basic_string(value) && !closes_multiline_basic_string(value);
    bool needs_balanced_collection =
        (!value.empty() && (value.at(0) == '[' || value.at(0) == '{')) && !top_level_delimiters_balanced(value);
    if (!needs_multiline_string && !needs_balanced_collection) return value;

    std::string_view continued{};
    while (reader.next_line(continued)) {
        value += "\n";
        value += strip_toml_comment(continued);
        if (needs_multiline_string && value.find("\"\"\"", 3) != std::string::npos) return value;
        if (needs_balanced_collection && top_level_delimiters_balanced(value)) return value;
    }
    return std::unexpected(ManifestError{manifest_path + ":" + std::to_string(reader.line_number) +
                        (needs_multiline_string ? ": unterminated multiline TOML string"
                                                : ": unterminated multiline TOML collection")});
}

[[nodiscard]] std::expected<int, ManifestError> parse_int_literal(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.empty()) return std::unexpected(ManifestError{context + " must be an integer"});
    std::string_view digits{value};
    if (!digits.empty() && digits.at(0) == '+') {
        digits = digits.substr(1);
    }
    int result = 0;
    bool ok = false;
    [[scpp::unsafe]] {
        const char* first = digits.data();
        const char* last = first + digits.size();
        std::from_chars_result res = std::from_chars(first, last, result);
        ok = static_cast<int>(res.ec) == 0 && res.ptr == last;
    }
    if (!ok) {
        return std::unexpected(ManifestError{context + " must be an integer"});
    }
    return result;
}

std::vector<std::string> split_top_level(std::string_view text, char delimiter) {
    std::vector<std::string> parts{};
    std::size_t start = 0;
    bool in_string = false;
    bool escape = false;
    int brace_depth = 0;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < text.size(); i++) {
        char ch = text.at(i);
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
    if (value.size() < 2 || value.at(0) != '[' || value.at(value.size() - 1) != ']') {
        return std::unexpected(ManifestError{context + " must be an array of strings"});
    }
    std::vector<std::string> entries = split_top_level(std::string_view{value}.substr(1, value.size() - 2), ',');
    std::vector<std::string> items{};
    items.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); i++) {
        auto item_result = parse_string_literal(entries[i], context);
        if (!item_result.has_value()) return std::unexpected(std::move(item_result).error());
        items.push_back(std::move(item_result).value());
    }
    return items;
}

[[nodiscard]] std::expected<std::vector<std::string>, ManifestError> parse_string_or_array(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (!value.empty() && value.at(0) == '[') return parse_string_array(value, context);
    auto item_result = parse_string_literal(value, context);
    if (!item_result.has_value()) return std::unexpected(std::move(item_result).error());
    std::vector<std::string> res{};
    res.push_back(std::move(item_result).value());
    return res;
}

[[nodiscard]] std::expected<StringMap<std::string>, ManifestError> parse_inline_table(std::string_view text, const std::string& context) {
    std::string value = trim(text);
    if (value.size() < 2 || value.at(0) != '{' || value.at(value.size() - 1) != '}') {
        return std::unexpected(ManifestError{context + " must be an inline table"});
    }
    std::vector<std::string> entries = split_top_level(std::string_view{value}.substr(1, value.size() - 2), ',');
    StringMap<std::string> table{};
    for (std::size_t i = 0; i < entries.size(); i++) {
        const std::string& entry = entries[i];
        std::size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(ManifestError{context + " contains malformed inline table entry '" + entry + "'"});
        }
        std::string key = trim(entry.substr(0, eq));
        std::string raw_value = trim(entry.substr(eq + 1));
        if (key.empty()) return std::unexpected(ManifestError{context + " contains an empty inline table key"});
        table.emplace(key, raw_value);
    }
    return std::move(table);
}

struct StatementGuard {
    sqlite3_stmt* stmt = nullptr;
    ~StatementGuard() {
        if (this->stmt != nullptr) {
            [[scpp::unsafe]] {
                sqlite3_finalize(this->stmt);
            }
        }
    }
};

class BuildRecord {
public:
    virtual ~BuildRecord() = default;
    BuildRecord() = default;
    BuildRecord(std::string k, std::string kd, std::string sig, std::string if_dig,
                std::string ar_dig, std::string out_dig, std::string out_p,
                std::string man_dig, std::string comp_v, std::string tr)
        : key{std::move(k)}, kind{std::move(kd)}, signature{std::move(sig)},
          interface_digest{std::move(if_dig)}, archive_digest{std::move(ar_dig)},
          output_digest{std::move(out_dig)}, output_path{std::move(out_p)},
          manifest_digest{std::move(man_dig)}, compiler_version{std::move(comp_v)},
          triple{std::move(tr)} {}

    std::string key{};
    std::string kind{};
    std::string signature{};
    std::string interface_digest{};
    std::string archive_digest{};
    std::string output_digest{};
    std::string output_path{};
    std::string manifest_digest{};
    std::string compiler_version{};
    std::string triple{};
};

class BuildDatabase {
public:
    explicit BuildDatabase(const std::string& db_path) : db_path_{normalized_path(db_path)} {}

    BuildDatabase(const BuildDatabase&) = delete;
    BuildDatabase& operator=(const BuildDatabase&) = delete;

    virtual ~BuildDatabase() {
        if (this->db_ != nullptr) {
            [[scpp::unsafe]] {
                sqlite3_close(this->db_);
            }
        }
    }

    [[nodiscard]] std::expected<void, BuildError> open() {
        std::string parent = path_parent(this->db_path_);
        if (!path_create_directories(parent)) {
            return std::unexpected(BuildError{"cannot create build database directory '" + parent + "'"});
        }
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_open(this->db_path_.c_str(), &this->db_);
        }
        if (rc != SQLITE_OK || this->db_ == nullptr) {
            std::string message = "cannot open build database '" + this->db_path_ + "'";
            if (this->db_ != nullptr) {
                [[scpp::unsafe]] {
                    const char* errmsg = sqlite3_errmsg(this->db_);
                    if (errmsg != nullptr) message += ": " + std::string{errmsg};
                }
            }
            return std::unexpected(BuildError{message});
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
        auto prepare_result = prepare(
            "SELECT key, kind, signature, interface_digest, archive_digest, output_digest, output_path, "
            "manifest_digest, compiler_version, triple FROM build_records WHERE key = ?1;");
        if (!prepare_result.has_value()) return std::unexpected(std::move(prepare_result).error());
        sqlite3_stmt* stmt = prepare_result.value();
        StatementGuard guard{stmt};
        if (auto bind_result = bind_text(stmt, 1, key); !bind_result.has_value()) {
            return std::unexpected(std::move(bind_result).error());
        }
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_step(stmt);
        }
        if (rc == SQLITE_ROW) {
            BuildRecord rec{};
            rec.key = column_text(stmt, 0);
            rec.kind = column_text(stmt, 1);
            rec.signature = column_text(stmt, 2);
            rec.interface_digest = column_text(stmt, 3);
            rec.archive_digest = column_text(stmt, 4);
            rec.output_digest = column_text(stmt, 5);
            rec.output_path = column_text(stmt, 6);
            rec.manifest_digest = column_text(stmt, 7);
            rec.compiler_version = column_text(stmt, 8);
            rec.triple = column_text(stmt, 9);
            return std::optional<BuildRecord>{std::move(rec)};
        }
        if (rc != SQLITE_DONE) {
            std::string err{};
            [[scpp::unsafe]] {
                const char* errmsg = sqlite3_errmsg(this->db_);
                if (errmsg != nullptr) err = std::string{errmsg};
            }
            return std::unexpected(BuildError{"build database query failed: " + err});
        }
        return std::optional<BuildRecord>{std::nullopt};
    }

    [[nodiscard]] std::expected<void, BuildError> put(const BuildRecord& record) {
        auto prepare_result = prepare(
            "INSERT INTO build_records "
            "(key, kind, signature, interface_digest, archive_digest, output_digest, output_path, "
            "manifest_digest, compiler_version, triple) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
            "ON CONFLICT(key) DO UPDATE SET "
            "kind=excluded.kind, signature=excluded.signature, interface_digest=excluded.interface_digest, "
            "archive_digest=excluded.archive_digest, output_digest=excluded.output_digest, "
            "output_path=excluded.output_path, manifest_digest=excluded.manifest_digest, "
            "compiler_version=excluded.compiler_version, triple=excluded.triple;");
        if (!prepare_result.has_value()) return std::unexpected(std::move(prepare_result).error());
        sqlite3_stmt* stmt = prepare_result.value();
        StatementGuard guard{stmt};
        if (auto r = bind_text(stmt, 1, record.key); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 2, record.kind); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 3, record.signature); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 4, record.interface_digest); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 5, record.archive_digest); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 6, record.output_digest); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 7, record.output_path); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 8, record.manifest_digest); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 9, record.compiler_version); !r.has_value()) return r;
        if (auto r = bind_text(stmt, 10, record.triple); !r.has_value()) return r;
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_step(stmt);
        }
        if (rc != SQLITE_DONE) {
            std::string err{};
            [[scpp::unsafe]] {
                const char* errmsg = sqlite3_errmsg(this->db_);
                if (errmsg != nullptr) err = std::string{errmsg};
            }
            return std::unexpected(BuildError{"build database write failed: " + err});
        }
        return {};
    }

private:
    [[nodiscard]] std::expected<void, BuildError> exec(const char* sql) {
        char* error = nullptr;
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_exec(this->db_, sql, nullptr, nullptr, &error);
        }
        if (rc != SQLITE_OK) {
            std::string message{"build database initialization failed"};
            if (error != nullptr) {
                std::string err{};
                [[scpp::unsafe]] {
                    err = std::string{error};
                    sqlite3_free(error);
                }
                message += ": " + err;
            }
            return std::unexpected(BuildError{message});
        }
        return {};
    }

    [[nodiscard]] std::expected<sqlite3_stmt*, BuildError> prepare(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_prepare_v2(this->db_, sql, -1, &stmt, nullptr);
        }
        if (rc != SQLITE_OK || stmt == nullptr) {
            std::string err{};
            if (this->db_ != nullptr) {
                [[scpp::unsafe]] {
                    const char* errmsg = sqlite3_errmsg(this->db_);
                    if (errmsg != nullptr) err = std::string{errmsg};
                }
            }
            return std::unexpected(BuildError{"build database statement preparation failed: " + err});
        }
        return stmt;
    }

    [[nodiscard]] std::expected<void, BuildError> bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
        int rc = -1;
        [[scpp::unsafe]] {
            rc = sqlite3_bind_text(stmt, index, value.c_str(), -1, nullptr);
        }
        if (rc != SQLITE_OK) {
            std::string err{};
            if (this->db_ != nullptr) {
                [[scpp::unsafe]] {
                    const char* errmsg = sqlite3_errmsg(this->db_);
                    if (errmsg != nullptr) err = std::string{errmsg};
                }
            }
            return std::unexpected(BuildError{"build database bind failed: " + err});
        }
        return {};
    }

    std::string column_text(sqlite3_stmt* stmt, int index) const {
        const char* text = nullptr;
        [[scpp::unsafe]] {
            text = sqlite3_column_text(stmt, index);
        }
        return text == nullptr ? std::string{} : std::string{text};
    }

    std::string db_path_{};
    sqlite3* db_ = nullptr;
};

class ManifestData;

std::string manifest_digest(const ManifestData& manifest);
std::string compiler_version_key();

void print_diagnostic(std::string_view path, const std::string& source, scpp::SourceLocation loc,
                      const std::string& message) {
    eprint(path);
    eprint(":");
    if (loc.is_known()) {
        eprint(std::to_string(static_cast<std::int64_t>(loc.line)) + ":" + std::to_string(static_cast<std::int64_t>(loc.column)) + ":");
    }
    eprint(" error: " + message + "\n");
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
    std::string_view line_text = std::string_view{source}.substr(line_start, line_end - line_start);
    std::string line_num = std::to_string(static_cast<std::int64_t>(loc.line));
    std::string gutter{};
    for (std::size_t i = 0; i < line_num.size(); i++) {
        gutter.push_back(' ');
    }
    eprintln(" " + line_num + " | " + string_from_view(line_text));
    eprint(" " + gutter + " | ");
    for (int i = 0; i < loc.column - 1 && static_cast<std::size_t>(i) < line_text.size(); i++) {
        eprint(line_text.at(static_cast<std::size_t>(i)) == '\t' ? "\t" : " ");
    }
    eprint("^\n");
}

constexpr int kManifestBuildOptLevel = 0;
constexpr bool kManifestBuildEmitDebugInfo = true;

class DependencySpec {
public:
    virtual ~DependencySpec() = default;
    std::string alias{};
    std::string path{};
};

class WorkspaceConfig {
public:
    virtual ~WorkspaceConfig() = default;
    std::vector<std::string> members{};
    std::vector<std::string> default_members{};
    bool has_default_members = false;
    bool has_workspace_dependencies = false;
};

class NativeRequirements {
public:
    virtual ~NativeRequirements() = default;
    std::vector<std::string> links{};
    std::vector<std::string> search_paths{};
};

class CustomCommand {
public:
    virtual ~CustomCommand() = default;
    CustomCommand() = default;
    CustomCommand(const CustomCommand& other) {
        for (std::size_t i = 0; i < other.input_paths.size(); i++) {
            this->input_paths.push_back(other.input_paths[i]);
        }
        for (std::size_t i = 0; i < other.output_paths.size(); i++) {
            this->output_paths.push_back(other.output_paths[i]);
        }
        this->command = other.command;
    }
    void operator=(const CustomCommand& other) {
        this->input_paths.clear();
        for (std::size_t i = 0; i < other.input_paths.size(); i++) {
            this->input_paths.push_back(other.input_paths[i]);
        }
        this->output_paths.clear();
        for (std::size_t i = 0; i < other.output_paths.size(); i++) {
            this->output_paths.push_back(other.output_paths[i]);
        }
        this->command = other.command;
    }
    std::vector<std::string> input_paths{};
    std::vector<std::string> output_paths{};
    std::string command{};
};

class ManifestTarget {
public:
    virtual ~ManifestTarget() = default;
    ManifestTarget() = default;
    ManifestTarget(const ManifestTarget& other) {
        this->name = other.name;
        for (std::size_t i = 0; i < other.source_patterns.size(); i++) {
            this->source_patterns.push_back(other.source_patterns[i]);
        }
        for (std::size_t i = 0; i < other.additional_obj_steps.size(); i++) {
            this->additional_obj_steps.push_back(other.additional_obj_steps[i]);
        }
    }
    void operator=(const ManifestTarget& other) {
        this->name = other.name;
        this->source_patterns.clear();
        for (std::size_t i = 0; i < other.source_patterns.size(); i++) {
            this->source_patterns.push_back(other.source_patterns[i]);
        }
        this->additional_obj_steps.clear();
        for (std::size_t i = 0; i < other.additional_obj_steps.size(); i++) {
            this->additional_obj_steps.push_back(other.additional_obj_steps[i]);
        }
    }
    std::string name{};
    std::vector<std::string> source_patterns{};
    std::vector<std::string> additional_obj_steps{};
};

class ManifestData {
public:
    virtual ~ManifestData() = default;
    int manifest_version = -1;
    std::optional<std::string> package_name{};
    std::optional<std::string> package_version{};
    std::vector<ManifestTarget> lib_targets{};
    std::vector<ManifestTarget> bin_targets{};
    StringMap<CustomCommand> custom_commands{};
    std::string manifest_path{};
    std::optional<WorkspaceConfig> workspace{};
    std::vector<DependencySpec> dependencies{};
    NativeRequirements native{};
};

class SourceInfo {
public:
    enum class Kind {
        Plain,
        PrimaryInterface,
        InterfacePartition,
        ImplementationUnit,
        ImplementationPartition
    };

    virtual ~SourceInfo() = default;
    SourceInfo() = default;
    SourceInfo(const SourceInfo& other) {
        this->path = other.path;
        this->kind = other.kind;
        this->module_name = other.module_name;
        this->partition_name = other.partition_name;
        for (std::size_t i = 0; i < other.imported_modules.size(); i++) {
            this->imported_modules.push_back(other.imported_modules[i]);
        }
    }
    std::string path{};
    Kind kind = Kind::Plain;
    std::string module_name{};
    std::string partition_name{};
    std::vector<std::string> imported_modules{};
};

class ScannedModuleDecl {
public:
    virtual ~ScannedModuleDecl() = default;
    std::string module_name{};
    std::string partition_name{};
    bool is_interface = false;
};

class BuiltModule {
public:
    virtual ~BuiltModule() = default;
    BuiltModule() = default;
    BuiltModule(const BuiltModule& other)
        : name{other.name}, source_path{other.source_path}, interface_path{other.interface_path},
          archive_path{other.archive_path}, interface_digest{other.interface_digest}, archive_digest{other.archive_digest} {}
    void operator=(const BuiltModule& other) {
        this->name = other.name;
        this->source_path = other.source_path;
        this->interface_path = other.interface_path;
        this->archive_path = other.archive_path;
        this->interface_digest = other.interface_digest;
        this->archive_digest = other.archive_digest;
    }
    BuiltModule(std::string n, std::string src, std::string iface, std::string arch,
                std::string if_dig, std::string ar_dig)
        : name{std::move(n)}, source_path{std::move(src)}, interface_path{std::move(iface)},
          archive_path{std::move(arch)}, interface_digest{std::move(if_dig)}, archive_digest{std::move(ar_dig)} {}

    std::string name{};
    std::string source_path{};
    std::string interface_path{};
    std::string archive_path{};
    std::string interface_digest{};
    std::string archive_digest{};
};

class BuildOutputs {
public:
    virtual ~BuildOutputs() = default;
    std::vector<BuiltModule> library_modules{};
    std::vector<std::string> binaries{};
};

class WorkspaceInfo {
public:
    virtual ~WorkspaceInfo() = default;
    ManifestData manifest{};
    std::vector<ManifestData> member_manifests{};
    std::vector<std::string> default_package_manifests{};
};

class PackageBuildResult {
public:
    virtual ~PackageBuildResult() = default;
    ManifestData manifest{};
    std::string package_output_root{};
    std::vector<BuiltModule> library_modules{};
    std::vector<std::string> binaries{};
    StringMap<std::vector<std::string>> custom_outputs{};
    StringMap<std::string> exported_modules{};
    StringMap<std::string> closure_import_paths{};
    StringMap<std::string> closure_module_owners{};
    std::vector<std::string> archive_closure{};
    std::vector<std::string> native_link_inputs{};
    bool uses_stdlib = false;
};

class ProjectDiscovery {
public:
    virtual ~ProjectDiscovery() = default;
    std::vector<std::string> manifests_nearest_first{};
    std::optional<std::string> current_manifest_path{};
    std::optional<std::string> workspace_manifest_path{};
};

enum class ManifestSection {
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
    Ignored
};

[[nodiscard]] std::expected<ManifestData, ManifestError> parse_manifest(const std::string& manifest_path) {
    ManifestData manifest{};
    manifest.manifest_path = normalized_path(manifest_path);

    auto file_res = read_file(manifest.manifest_path);
    if (!file_res.has_value()) {
        return std::unexpected(ManifestError{"cannot open manifest '" + manifest.manifest_path + "'"});
    }
    const std::string& file_content = file_res.value();

    ManifestSection current_section = ManifestSection::Root;
    std::string current_custom{};
    int current_bin_idx = -1;
    int current_lib_idx = -1;

    LineReader reader{file_content, 0, 0};
    std::string_view line_view{};
    while (reader.next_line(line_view)) {
        std::string stripped = trim(strip_toml_comment(line_view));
        if (stripped.empty()) continue;
        if (stripped.at(0) == '[') {
            if (stripped.size() < 2 || stripped.at(stripped.size() - 1) != ']') {
                return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                    ": malformed section header"});
            }
            current_bin_idx = -1;
            current_lib_idx = -1;
            if (stripped.size() >= 2 && stripped[0] == '[' && stripped[1] == '[') {
                if (stripped.size() < 4 || stripped.substr(stripped.size() - 2) != "]]" ) {
                    return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                        ": malformed array-of-table header"});
                }
                std::string section_name = trim(stripped.substr(2, stripped.size() - 4));
                if (section_name == "bin") {
                    manifest.bin_targets.push_back(ManifestTarget{});
                    current_bin_idx = static_cast<int>(manifest.bin_targets.size()) - 1;
                    current_section = ManifestSection::Bin;
                    continue;
                }
                if (section_name == "lib") {
                    manifest.lib_targets.push_back(ManifestTarget{});
                    current_lib_idx = static_cast<int>(manifest.lib_targets.size()) - 1;
                    current_section = ManifestSection::Lib;
                    continue;
                }
                return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                    ": unsupported array-of-table [[" + section_name + "]]"});
            }
            std::string section_name = trim(stripped.substr(1, stripped.size() - 2));
            if (section_name == "package") {
                current_section = ManifestSection::Package;
            } else if (section_name == "lib") {
                return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                    ": [lib] has been replaced by [[lib]]"});
            } else if (section_name == "dependencies") {
                current_section = ManifestSection::Dependencies;
            } else if (section_name == "native") {
                current_section = ManifestSection::Native;
            } else if (section_name.starts_with("additional_objs.")) {
                current_section = ManifestSection::Custom;
                current_custom = section_name.substr(std::string{"additional_objs."}.size());
                if (current_custom.empty()) {
                    return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                        ": additional_objs section name cannot be empty"});
                }
                if (!manifest.custom_commands.contains(current_custom)) {
                    manifest.custom_commands.emplace(current_custom, CustomCommand{});
                }
            } else if (section_name == "workspace") {
                current_section = ManifestSection::Workspace;
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
            } else if (section_name == "workspace.dependencies") {
                current_section = ManifestSection::WorkspaceDependencies;
                if (!manifest.workspace.has_value()) manifest.workspace = WorkspaceConfig{};
                manifest.workspace->has_workspace_dependencies = true;
            } else if (section_name == "package.metadata") {
                current_section = ManifestSection::PackageMetadata;
            } else {
                current_section = ManifestSection::Ignored;
            }
            continue;
        }

        std::size_t eq = stripped.find('=');
        if (eq == std::string::npos) {
            return std::unexpected(ManifestError{manifest.manifest_path + ":" + std::to_string(reader.line_number) +
                                ": expected key = value"});
        }
        std::string key = trim(stripped.substr(0, eq));
        auto value_result = read_manifest_value(stripped.substr(eq + 1), reader, manifest.manifest_path);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::string value = std::move(value_result).value();
        std::string context = manifest.manifest_path + ":" + std::to_string(reader.line_number) + ": " + key;

        switch (current_section) {
            case ManifestSection::Root:
                if (key == "manifest-version") {
                    auto r = parse_int_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.manifest_version = r.value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in the manifest root"});
                }
                break;
            case ManifestSection::Package:
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.package_name = std::move(r).value();
                } else if (key == "version") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.package_version = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [package]"});
                }
                break;
            case ManifestSection::Lib:
                if (current_lib_idx < 0) return std::unexpected(ManifestError{context + " is outside a [[lib]] table"});
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.lib_targets[static_cast<std::size_t>(current_lib_idx)].name = std::move(r).value();
                } else if (key == "sources") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.lib_targets[static_cast<std::size_t>(current_lib_idx)].source_patterns = std::move(r).value();
                } else if (key == "additional_objs") {
                    auto r = parse_string_or_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.lib_targets[static_cast<std::size_t>(current_lib_idx)].additional_obj_steps = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [[lib]]"});
                }
                break;
            case ManifestSection::Bin:
                if (current_bin_idx < 0) return std::unexpected(ManifestError{context + " is outside a [[bin]] table"});
                if (key == "name") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.bin_targets[static_cast<std::size_t>(current_bin_idx)].name = std::move(r).value();
                } else if (key == "sources") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.bin_targets[static_cast<std::size_t>(current_bin_idx)].source_patterns = std::move(r).value();
                } else if (key == "additional_objs") {
                    auto r = parse_string_or_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.bin_targets[static_cast<std::size_t>(current_bin_idx)].additional_obj_steps = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [[bin]]"});
                }
                break;
            case ManifestSection::Custom:
                if (!manifest.custom_commands.contains(current_custom)) {
                    return std::unexpected(ManifestError{context + " is outside an [additional_objs." + current_custom + "]"});
                }
                if (key == "input") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands.at(current_custom).input_paths = std::move(r).value();
                } else if (key == "output") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands.at(current_custom).output_paths = std::move(r).value();
                } else if (key == "command") {
                    auto r = parse_string_literal(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.custom_commands.at(current_custom).command = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [additional_objs." + current_custom + "]"});
                }
                break;
            case ManifestSection::Dependencies: {
                auto table_result = parse_inline_table(value, context);
                if (!table_result.has_value()) return std::unexpected(std::move(table_result).error());
                StringMap<std::string> table = std::move(table_result).value();
                if (table.contains("path")) {
                    DependencySpec dep{};
                    dep.alias = key;
                    auto path_result = parse_string_literal(table.at("path"), context + " path");
                    if (!path_result.has_value()) return std::unexpected(std::move(path_result).error());
                    dep.path = std::move(path_result).value();
                    if (table.size() != 1) {
                        return std::unexpected(ManifestError{context + " currently supports only { path = \"...\" }"});
                    }
                    manifest.dependencies.push_back(std::move(dep));
                } else if (table.contains("scppkg") || table.contains("workspace") || table.contains("git") ||
                           table.contains("version")) {
                    return std::unexpected(ManifestError{context + " uses a dependency source that is designed but not implemented yet"});
                } else {
                    return std::unexpected(ManifestError{context + " must specify { path = \"...\" }"});
                }
                break;
            }
            case ManifestSection::Native:
                if (key == "links") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.native.links = std::move(r).value();
                } else if (key == "search") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.native.search_paths = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [native]"});
                }
                break;
            case ManifestSection::Workspace:
                if (key == "members") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.workspace->members = std::move(r).value();
                } else if (key == "default-members") {
                    auto r = parse_string_array(value, context);
                    if (!r.has_value()) return std::unexpected(std::move(r).error());
                    manifest.workspace->has_default_members = true;
                    manifest.workspace->default_members = std::move(r).value();
                } else {
                    return std::unexpected(ManifestError{context + " is not supported in [workspace]"});
                }
                break;
            case ManifestSection::WorkspaceDependencies:
                manifest.workspace->has_workspace_dependencies = true;
                break;
            case ManifestSection::PackageMetadata:
            case ManifestSection::Ignored:
                break;
        }
    }

    if (manifest.manifest_version != 1) {
        return std::unexpected(ManifestError{"manifest-version = 1 is required in '" + manifest.manifest_path + "'"});
    }
    if (manifest.workspace.has_value() && manifest.workspace->has_workspace_dependencies) {
        return std::unexpected(ManifestError{"[workspace.dependencies] is designed but not implemented yet"});
    }
    if (!manifest.package_name.has_value() && !manifest.workspace.has_value()) {
        return std::unexpected(ManifestError{"manifest must declare either [package] or [workspace]"});
    }
    if (!manifest.package_name.has_value()) {
        if (!manifest.lib_targets.empty() || !manifest.bin_targets.empty() || !manifest.dependencies.empty()) {
            return std::unexpected(ManifestError{"a virtual workspace manifest cannot declare [[lib]], [[bin]], or [dependencies]"});
        }
        return std::move(manifest);
    }
    if (manifest.lib_targets.empty() && manifest.bin_targets.empty()) {
        return std::unexpected(ManifestError{"manifest must declare at least one [[lib]] or [[bin]] target"});
    }
    StringSet lib_names{};
    for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
        const ManifestTarget& lib = manifest.lib_targets[i];
        if (lib.name.empty()) return std::unexpected(ManifestError{"[[lib]].name is required"});
        if (lib.source_patterns.empty()) return std::unexpected(ManifestError{"[[lib]].sources is required"});
        if (!lib_names.insert(lib.name).second) {
            return std::unexpected(ManifestError{"duplicate [[lib]] target name '" + lib.name + "'"});
        }
    }
    StringSet bin_names{};
    for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
        const ManifestTarget& bin = manifest.bin_targets[i];
        if (bin.name.empty()) return std::unexpected(ManifestError{"[[bin]].name is required"});
        if (bin.source_patterns.empty()) return std::unexpected(ManifestError{"[[bin]].sources is required"});
        if (!bin_names.insert(bin.name).second) {
            return std::unexpected(ManifestError{"duplicate [[bin]] target name '" + bin.name + "'"});
        }
    }
    for (std::size_t i = 0; i < manifest.custom_commands.size(); i++) {
        const std::string& name = manifest.custom_commands.keys_[i];
        const CustomCommand& custom = manifest.custom_commands.values_[i];
        if (custom.input_paths.empty()) return std::unexpected(ManifestError{"[additional_objs." + name + "].input is required"});
        if (custom.output_paths.empty()) return std::unexpected(ManifestError{"[additional_objs." + name + "].output is required"});
        if (custom.command.empty()) return std::unexpected(ManifestError{"[additional_objs." + name + "].command is required"});
    }
    for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
        const ManifestTarget& target = manifest.lib_targets[i];
        StringSet seen{};
        for (std::size_t j = 0; j < target.additional_obj_steps.size(); j++) {
            const std::string& step_name = target.additional_obj_steps[j];
            if (!manifest.custom_commands.contains(step_name)) {
                return std::unexpected(ManifestError{"[[lib]] target '" + target.name +
                                    "' references unknown [additional_objs." + step_name + "]"});
            }
            if (!seen.insert(step_name).second) {
                return std::unexpected(ManifestError{"[[lib]] target '" + target.name +
                                    "' references duplicate custom step '" + step_name + "'"});
            }
        }
    }
    for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
        const ManifestTarget& target = manifest.bin_targets[i];
        StringSet seen{};
        for (std::size_t j = 0; j < target.additional_obj_steps.size(); j++) {
            const std::string& step_name = target.additional_obj_steps[j];
            if (!manifest.custom_commands.contains(step_name)) {
                return std::unexpected(ManifestError{"[[bin]] target '" + target.name +
                                    "' references unknown [additional_objs." + step_name + "]"});
            }
            if (!seen.insert(step_name).second) {
                return std::unexpected(ManifestError{"[[bin]] target '" + target.name +
                                    "' references duplicate custom step '" + step_name + "'"});
            }
        }
    }
    return std::move(manifest);
}

[[nodiscard]] std::expected<std::optional<ScannedModuleDecl>, BuildError> scan_declared_module(
    const std::vector<scpp::Token>& tokens, const std::string& path_for_errors) {
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

    ScannedModuleDecl decl{};
    decl.module_name = string_from_view(tokens[i].text);
    decl.is_interface = exported_module;
    i++;
    while (i + 1 < tokens.size() && tokens[i].kind == scpp::TokenKind::Dot &&
           tokens[i + 1].kind == scpp::TokenKind::Identifier) {
        decl.module_name += ".";
        decl.module_name += string_from_view(tokens[i + 1].text);
        i += 2;
    }
    if (i < tokens.size() && tokens[i].kind == scpp::TokenKind::Colon) {
        i++;
        if (i >= tokens.size() || tokens[i].kind != scpp::TokenKind::Identifier) {
            return std::unexpected(BuildError{"invalid partition declaration in '" + path_for_errors + "'"});
        }
        decl.partition_name = string_from_view(tokens[i].text);
    }
    return std::optional<ScannedModuleDecl>{std::move(decl)};
}

[[nodiscard]] std::expected<SourceInfo, BuildError> classify_source(const std::string& path) {
    SourceInfo info{};
    info.path = normalized_path(path);
    auto source_result = read_file(info.path);
    if (!source_result.has_value()) return std::unexpected(std::move(source_result).error());
    std::vector<scpp::Token> tokens = scpp::tokenize(std::string_view{source_result.value()});
    auto decl_result = scan_declared_module(tokens, info.path);
    if (!decl_result.has_value()) return std::unexpected(std::move(decl_result).error());
    if (decl_result.value().has_value()) {
        const ScannedModuleDecl& decl = decl_result.value().value();
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
        std::string module_name = string_from_view(tokens[j].text);
        j++;
        while (j + 1 < tokens.size() && tokens[j].kind == scpp::TokenKind::Dot &&
               tokens[j + 1].kind == scpp::TokenKind::Identifier) {
            module_name += ".";
            module_name += string_from_view(tokens[j + 1].text);
            j += 2;
        }
        info.imported_modules.push_back(module_name);
    }
    return std::move(info);
}

[[nodiscard]] std::expected<std::vector<std::string>, BuildError> topo_sort_modules(
    const StringMap<SourceInfo>& primary_modules) {
    StringMap<std::vector<std::string>> edges{};
    StringMap<int> indegree{};
    for (std::size_t i = 0; i < primary_modules.size(); i++) {
        indegree.emplace(primary_modules.keys_[i], 0);
        std::vector<std::string> empty_edges{};
        edges.emplace(primary_modules.keys_[i], empty_edges);
    }
    for (std::size_t i = 0; i < primary_modules.size(); i++) {
        const std::string& name = primary_modules.keys_[i];
        const SourceInfo& source = primary_modules.values_[i];
        std::vector<std::string> local_deps{};
        for (std::size_t j = 0; j < source.imported_modules.size(); j++) {
            const std::string& imported = source.imported_modules[j];
            if (primary_modules.contains(imported) && imported != name) {
                if (!vector_contains(local_deps, imported)) local_deps.push_back(imported);
            }
        }
        for (std::size_t j = 0; j < local_deps.size(); j++) {
            edges.at(local_deps[j]).push_back(name);
            indegree.at(name) = indegree.at(name) + 1;
        }
    }
    std::vector<std::string> ready{};
    for (std::size_t i = 0; i < indegree.size(); i++) {
        if (indegree.values_[i] == 0) ready.push_back(indegree.keys_[i]);
    }
    sort_vector(ready);
    std::vector<std::string> order{};
    while (!ready.empty()) {
        std::string current{ready[0]};
        for (std::size_t k = 0; k + 1 < ready.size(); k++) {
            ready[k] = ready[k + 1];
        }
        ready.pop_back();
        order.push_back(current);
        if (edges.contains(current)) {
            const std::vector<std::string>& dependents = edges.at(current);
            for (std::size_t j = 0; j < dependents.size(); j++) {
                const std::string& dependent = dependents[j];
                indegree.at(dependent) = indegree.at(dependent) - 1;
                if (indegree.at(dependent) == 0) {
                    ready.push_back(dependent);
                    sort_vector(ready);
                }
            }
        }
    }
    if (order.size() != primary_modules.size()) {
        return std::unexpected(BuildError{"cyclic local module dependency detected in manifest target source set"});
    }
    return order;
}

[[nodiscard]] std::expected<std::vector<SourceInfo>, BuildError> classify_target_sources(
    const std::string& manifest_dir, const ManifestTarget& target) {
    std::vector<std::string> source_paths = expand_source_patterns(manifest_dir, target.source_patterns);
    if (source_paths.empty()) {
        return std::unexpected(BuildError{"target sources globs matched no files"});
    }
    std::vector<SourceInfo> sources{};
    for (std::size_t i = 0; i < source_paths.size(); i++) {
        auto info_result = classify_source(source_paths[i]);
        if (!info_result.has_value()) return std::unexpected(std::move(info_result).error());
        sources.push_back(std::move(info_result).value());
    }
    return std::move(sources);
}

StringSet local_primary_module_names(const std::vector<SourceInfo>& sources) {
    StringSet names{};
    for (std::size_t i = 0; i < sources.size(); i++) {
        if (sources[i].kind == SourceInfo::Kind::PrimaryInterface) names.insert(sources[i].module_name);
    }
    return names;
}

[[nodiscard]] std::expected<StringMap<std::string>, BuildError> local_source_import_paths(
    const std::vector<SourceInfo>& sources) {
    StringMap<std::string> import_paths{};
    for (std::size_t i = 0; i < sources.size(); i++) {
        const SourceInfo& source = sources[i];
        std::optional<std::string> key{};
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
        if (import_paths.contains(*key)) {
            if (import_paths.at(*key) != source.path) {
                return std::unexpected(BuildError{"multiple source files declare '" + *key + "' within one manifest target"});
            }
        } else {
            import_paths.emplace(*key, source.path);
        }
    }
    return std::move(import_paths);
}

bool source_uses_stdlib(const SourceInfo& source) {
    return vector_contains(source.imported_modules, std::string{"std"});
}

bool sources_use_stdlib(const std::vector<SourceInfo>& sources) {
    for (std::size_t i = 0; i < sources.size(); i++) {
        if (source_uses_stdlib(sources[i])) return true;
    }
    return false;
}

[[nodiscard]] std::expected<void, BuildError> validate_direct_visibility(
    const std::vector<SourceInfo>& sources,
    const StringSet& local_modules,
    const StringMap<std::string>& direct_modules,
    const StringMap<std::string>& transitive_only_modules) {
    for (std::size_t i = 0; i < sources.size(); i++) {
        const SourceInfo& source = sources[i];
        for (std::size_t j = 0; j < source.imported_modules.size(); j++) {
            const std::string& imported = source.imported_modules[j];
            if (imported == "std") continue;
            if (local_modules.contains(imported)) continue;
            if (direct_modules.contains(imported)) continue;
            if (transitive_only_modules.contains(imported)) {
                return std::unexpected(BuildError{"module '" + imported + "' is exported only by transitive dependency package '" +
                                 transitive_only_modules.at(imported) + "'; add it as a direct dependency to import it"});
            }
        }
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<BuiltModule>, BuildError> build_modules_for_target(
    const std::vector<SourceInfo>& sources,
    const std::string& module_dir,
    const std::string& archive_dir,
    const StringMap<std::string>& base_import_paths,
    int opt_level,
    const ManifestData& manifest,
    const ManifestTarget* target,
    BuildDatabase& database) {
    StringMap<SourceInfo> primary_modules{};
    for (std::size_t i = 0; i < sources.size(); i++) {
        const SourceInfo& source = sources[i];
        switch (source.kind) {
            case SourceInfo::Kind::PrimaryInterface:
                if (primary_modules.contains(source.module_name)) {
                    return std::unexpected(BuildError{"duplicate primary interface for module '" + source.module_name + "'"});
                }
                primary_modules.emplace(source.module_name, source);
                break;
            case SourceInfo::Kind::InterfacePartition:
            case SourceInfo::Kind::ImplementationPartition:
                break;
            case SourceInfo::Kind::ImplementationUnit:
                return std::unexpected(BuildError{"module implementation units are not implemented in project builds yet ('" +
                                 source.path + "')"});
            case SourceInfo::Kind::Plain:
                break;
        }
    }

    for (std::size_t i = 0; i < sources.size(); i++) {
        const SourceInfo& source = sources[i];
        if ((source.kind == SourceInfo::Kind::InterfacePartition ||
             source.kind == SourceInfo::Kind::ImplementationPartition) &&
            !primary_modules.contains(source.module_name)) {
            return std::unexpected(BuildError{"partition '" + source.module_name + ":" + source.partition_name +
                             "' has no primary interface in this target"});
        }
    }

    auto build_order_result = topo_sort_modules(primary_modules);
    if (!build_order_result.has_value()) return std::unexpected(std::move(build_order_result).error());
    std::vector<std::string> build_order = std::move(build_order_result).value();
    StringMap<std::string> import_paths{};
    for (std::size_t i = 0; i < base_import_paths.size(); i++) {
        import_paths.emplace(base_import_paths.keys_[i], base_import_paths.values_[i]);
    }
    auto local_import_paths_result = local_source_import_paths(sources);
    if (!local_import_paths_result.has_value()) return std::unexpected(std::move(local_import_paths_result).error());
    for (std::size_t i = 0; i < local_import_paths_result.value().size(); i++) {
        import_paths.emplace(local_import_paths_result.value().keys_[i], local_import_paths_result.value().values_[i]);
    }
    StringMap<int> indegree{};
    StringMap<std::vector<std::string>> dependents{};
    for (std::size_t i = 0; i < primary_modules.size(); i++) {
        indegree.emplace(primary_modules.keys_[i], 0);
        std::vector<std::string> empty_deps{};
        dependents.emplace(primary_modules.keys_[i], empty_deps);
    }
    for (std::size_t i = 0; i < primary_modules.size(); i++) {
        const std::string& name = primary_modules.keys_[i];
        const SourceInfo& source = primary_modules.values_[i];
        std::vector<std::string> local_deps{};
        for (std::size_t j = 0; j < source.imported_modules.size(); j++) {
            const std::string& imported = source.imported_modules[j];
            if (primary_modules.contains(imported) && imported != name) {
                if (!vector_contains(local_deps, imported)) local_deps.push_back(imported);
            }
        }
        for (std::size_t j = 0; j < local_deps.size(); j++) {
            dependents.at(local_deps[j]).push_back(name);
            indegree.at(name) = indegree.at(name) + 1;
        }
    }
    std::vector<BuiltModule> outputs{};
    std::vector<std::string> ready{};
    for (std::size_t i = 0; i < indegree.size(); i++) {
        if (indegree.values_[i] == 0) ready.push_back(indegree.keys_[i]);
    }
    sort_vector(ready);
    if (target != nullptr) {
        if (primary_modules.empty()) {
            return std::unexpected(BuildError{"[[lib]] target must contain at least one primary interface module"});
        }
        std::string target_name{};
        bool has_additional_objs = false;
        [[scpp::unsafe]] {
            target_name = target->name;
            has_additional_objs = !target->additional_obj_steps.empty();
        }
        if (primary_modules.size() != 1 && has_additional_objs) {
            return std::unexpected(BuildError{"[[lib]] target '" + target_name +
                             "' must contain exactly one primary interface module when using additional_objs "
                             "(archive merging can't tell which module's archive to merge native objects into)"});
        }
    }
    const std::string manifest_key = manifest_digest(manifest);
    const std::string compiler_key = compiler_version_key();
    StringMap<std::vector<std::string>> partition_paths_by_module{};
    for (std::size_t i = 0; i < sources.size(); i++) {
        const SourceInfo& source = sources[i];
        if (source.kind == SourceInfo::Kind::InterfacePartition || source.kind == SourceInfo::Kind::ImplementationPartition) {
            if (!partition_paths_by_module.contains(source.module_name)) {
                std::vector<std::string> empty_vec{};
                partition_paths_by_module.emplace(source.module_name, empty_vec);
            }
            partition_paths_by_module.at(source.module_name).push_back(source.path);
        }
    }
    while (!ready.empty()) {
        std::vector<std::string> batch = ready;
        ready.clear();
        for (std::size_t batch_idx = 0; batch_idx < batch.size(); batch_idx++) {
            const std::string& module_name = batch[batch_idx];
            const SourceInfo& source = primary_modules.at(module_name);
            std::string interface_path = path_join(module_dir, module_name + ".scppm");
            std::string target_name{};
            if (target != nullptr) {
                [[scpp::unsafe]] {
                    target_name = target->name;
                }
            }
            std::string archive_base_name = module_name;
            if (!target_name.empty() && primary_modules.size() == 1) {
                archive_base_name = target_name;
            }
            std::string archive_path = path_join(archive_dir, "lib" + archive_base_name + ".scppa");
            auto module_source_result = read_file(source.path);
            if (!module_source_result.has_value()) return std::unexpected(std::move(module_source_result).error());
            const std::string& module_source = module_source_result.value();
            std::vector<std::string> dep_keys{};
            for (std::size_t j = 0; j < source.imported_modules.size(); j++) {
                const std::string& imported = source.imported_modules[j];
                if (!import_paths.contains(imported)) continue;
                dep_keys.push_back(imported + "=" + import_paths.at(imported) + "#" + path_digest_or_empty(import_paths.at(imported)));
            }
            sort_vector(dep_keys);
            std::vector<std::string> module_source_paths{};
            module_source_paths.push_back(source.path);
            if (partition_paths_by_module.contains(module_name)) {
                const std::vector<std::string>& pparts = partition_paths_by_module.at(module_name);
                for (std::size_t j = 0; j < pparts.size(); j++) {
                    module_source_paths.push_back(pparts[j]);
                }
            }
            sort_vector(module_source_paths);
            std::vector<std::string> sig_items{};
            sig_items.push_back("kind=module");
            sig_items.push_back("source=" + join_for_digest(path_digests(module_source_paths)));
            sig_items.push_back("triple=" + scpp::host_target_triple());
            sig_items.push_back("compiler=" + compiler_key);
            sig_items.push_back("manifest=" + manifest_key);
            sig_items.push_back("opt=" + std::to_string(static_cast<std::int64_t>(opt_level)));
            sig_items.push_back("deps=" + join_for_digest(dep_keys));
            std::string signature = fnv1a64_hex(join_for_digest(sig_items));
            std::string record_key = "module|" + manifest.manifest_path + "|" + module_name;
            auto cached_result = database.get(record_key);
            if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
            if (cached_result.value().has_value() && cached_result.value().value().signature == signature &&
                path_exists(interface_path) && path_exists(archive_path)) {
                trace_build("cache hit module " + module_name);
                BuiltModule built{module_name, source.path, interface_path, archive_path,
                                  path_digest_or_empty(interface_path), path_digest_or_empty(archive_path)};
                import_paths.emplace(built.name, built.interface_path);
                outputs.push_back(std::move(built));
                continue;
            }
            trace_build("build module " + module_name);
            auto emit_r = scpp::emit_module_artifacts(module_source, interface_path, archive_path,
                                                      to_std_map(import_paths), {}, source.path, opt_level);
            if (!emit_r.has_value()) {
                print_diagnostic(source.path, module_source, emit_r.error().loc, emit_r.error().what());
                return std::unexpected(BuildError{emit_r.error().what()});
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
                archive_path,
                manifest_key,
                compiler_key,
                scpp::host_target_triple()
            }); !put_result.has_value()) {
                return std::unexpected(std::move(put_result).error());
            }
            import_paths.emplace(built.name, built.interface_path);
            outputs.push_back(std::move(built));
        }
        sort_vector_by(outputs, [&](const BuiltModule& lhs, const BuiltModule& rhs) {
            std::size_t lhs_idx = 0;
            std::size_t rhs_idx = 0;
            for (std::size_t i = 0; i < build_order.size(); i++) {
                if (build_order[i] == lhs.name) lhs_idx = i;
                if (build_order[i] == rhs.name) rhs_idx = i;
            }
            return lhs_idx < rhs_idx;
        });
        std::vector<std::string> newly_ready{};
        for (std::size_t batch_idx = 0; batch_idx < batch.size(); batch_idx++) {
            const std::string& module_name = batch[batch_idx];
            if (dependents.contains(module_name)) {
                const std::vector<std::string>& deps = dependents.at(module_name);
                for (std::size_t j = 0; j < deps.size(); j++) {
                    const std::string& dependent = deps[j];
                    indegree.at(dependent) = indegree.at(dependent) - 1;
                    if (indegree.at(dependent) == 0) newly_ready.push_back(dependent);
                }
            }
        }
        sort_vector(newly_ready);
        ready = std::move(newly_ready);
    }
    return std::move(outputs);
}

StringMap<std::string> to_import_map(const std::vector<BuiltModule>& modules) {
    StringMap<std::string> import_paths{};
    for (std::size_t i = 0; i < modules.size(); i++) {
        import_paths.emplace(modules[i].name, modules[i].interface_path);
    }
    return import_paths;
}

[[nodiscard]] std::expected<void, BuildError> append_import_maps(
    StringMap<std::string>& into,
    const StringMap<std::string>& extra,
    const StringMap<std::string>* owner_lookup = nullptr) {
    for (std::size_t i = 0; i < extra.size(); i++) {
        const std::string& name = extra.keys_[i];
        const std::string& path = extra.values_[i];
        if (into.contains(name)) {
            if (into.at(name) != path) {
                std::string detail{};
                if (owner_lookup != nullptr) {
                    bool has_owner = false;
                    std::string pkg{};
                    [[scpp::unsafe]] {
                        has_owner = owner_lookup->contains(name);
                        if (has_owner) pkg = owner_lookup->at(name);
                    }
                    if (has_owner) {
                        detail = " from package '" + pkg + "'";
                    }
                }
                return std::unexpected(BuildError{"module '" + name + "' is produced by multiple direct dependencies" + detail});
            }
        } else {
            into.emplace(name, path);
        }
    }
    return {};
}

std::optional<std::string> find_program_on_path(const std::string& program) {
    if (program.empty()) return std::nullopt;
    const char* path_env = nullptr;
    char path_first = '\0';
    [[scpp::unsafe]] {
        path_env = getenv("PATH");
        if (path_env != nullptr) path_first = path_env[0];
    }
    if (path_env == nullptr || path_first == '\0') return std::nullopt;
    std::string pview{path_env};
    std::size_t start = 0;
    while (start < pview.size()) {
        std::size_t colon = pview.find(':', start);
        std::string dir = (colon == std::string::npos) ? pview.substr(start) : pview.substr(start, colon - start);
        if (!dir.empty()) {
            std::string candidate = path_join(dir, program);
            int rc = -1;
            [[scpp::unsafe]] {
                rc = access(candidate.c_str(), 1);
            }
            if (rc == 0) return candidate;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return std::nullopt;
}

std::optional<std::string> default_custom_step_cxx() {
    const char* env = nullptr;
    char first = '\0';
    [[scpp::unsafe]] {
        env = getenv("CXX");
        if (env != nullptr) first = env[0];
    }
    if (env != nullptr && first != '\0') return std::string{env};
    std::vector<std::string> candidates{};
    candidates.push_back("clang++");
    candidates.push_back("clang++-22");
    candidates.push_back("g++");
    candidates.push_back("c++");
    for (std::size_t i = 0; i < candidates.size(); i++) {
        std::optional<std::string> resolved = find_program_on_path(candidates[i]);
        if (resolved.has_value()) {
            return resolved;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::expected<void, BuildError> prepare_custom_workdir(
    const std::string& manifest_dir, const std::string& work_dir) {
    if (!path_create_directories(work_dir)) {
        return std::unexpected(BuildError{"failed to create custom step workdir '" + work_dir + "'"});
    }
    void* d = nullptr;
    [[scpp::unsafe]] {
        d = opendir(manifest_dir.c_str());
    }
    if (d == nullptr) {
        return std::unexpected(BuildError{"failed to open manifest directory '" + manifest_dir + "'"});
    }
    while (true) {
        dirent* entry = nullptr;
        [[scpp::unsafe]] {
            entry = readdir(d);
        }
        if (entry == nullptr) break;
        std::string name{};
        [[scpp::unsafe]] {
            name = std::string{entry->d_name};
        }
        if (name == "." || name == ".." || name == ".scpp") continue;
        std::string source_entry = path_join(manifest_dir, name);
        std::string link_path = path_join(work_dir, name);
        path_remove_all(link_path);
        if (!path_symlink(source_entry, link_path)) {
            [[scpp::unsafe]] {
                closedir(d);
            }
            return std::unexpected(BuildError{"failed to prepare custom step workdir entry '" + link_path + "'"});
        }
    }
    [[scpp::unsafe]] {
        closedir(d);
    }
    return {};
}

std::vector<std::string> expand_native_link_inputs(const ManifestData& manifest) {
    std::vector<std::string> inputs{};
    std::string manifest_dir = path_parent(manifest.manifest_path);
    for (std::size_t i = 0; i < manifest.native.search_paths.size(); i++) {
        const std::string& path = manifest.native.search_paths[i];
        std::string resolved = (!path.empty() && path.at(0) == '/') ? path : normalized_path(path_join(manifest_dir, path));
        inputs.push_back("-L" + resolved);
    }
    for (std::size_t i = 0; i < manifest.native.links.size(); i++) {
        inputs.push_back("-l" + manifest.native.links[i]);
    }
    return inputs;
}

std::string manifest_digest(const ManifestData& manifest) {
    return digest_file(manifest.manifest_path);
}

std::optional<std::string> current_executable_path_local() {
    char buf[4096] = {};
    long len = 0;
    [[scpp::unsafe]] {
        len = readlink("/proc/self/exe", buf, 4095);
    }
    if (len <= 0) return std::nullopt;
    buf[len] = '\0';
    return normalized_path(std::string{buf, static_cast<std::size_t>(len)});
}

std::string compiler_version_key() {
    static std::string key{};
    if (key.empty()) {
        std::optional<std::string> exe = current_executable_path_local();
        if (exe.has_value()) {
            key = digest_file(*exe);
        } else {
            key = std::string{"unknown-compiler"};
        }
    }
    return key;
}

std::vector<std::string> manifests_upward(const std::string& start_dir) {
    std::vector<std::string> manifests{};
    std::string current = normalized_path(start_dir);
    while (true) {
        std::string candidate = path_join(current, "scpp.toml");
        if (path_exists(candidate)) manifests.push_back(normalized_path(candidate));
        if (current == "/" || current.empty()) break;
        std::string parent = path_parent(current);
        if (parent == current || parent == ".") break;
        current = parent;
    }
    return manifests;
}

[[nodiscard]] std::expected<ProjectDiscovery, ManifestError> discover_project(const std::string& start_dir) {
    ProjectDiscovery discovery{};
    std::vector<std::string> manifests = manifests_upward(start_dir);
    for (std::size_t i = 0; i < manifests.size(); i++) {
        discovery.manifests_nearest_first.push_back(manifests[i]);
        if (!discovery.current_manifest_path.has_value()) {
            auto manifest_result = parse_manifest(manifests[i]);
            if (!manifest_result.has_value()) return std::unexpected(std::move(manifest_result).error());
            const ManifestData& manifest = manifest_result.value();
            if (manifest.workspace.has_value()) {
                discovery.workspace_manifest_path = manifests[i];
            }
            discovery.current_manifest_path = manifests[i];
        } else if (!discovery.workspace_manifest_path.has_value()) {
            auto manifest_result = parse_manifest(manifests[i]);
            if (!manifest_result.has_value()) return std::unexpected(std::move(manifest_result).error());
            const ManifestData& manifest = manifest_result.value();
            if (manifest.workspace.has_value()) {
                discovery.workspace_manifest_path = manifests[i];
            }
        }
    }
    return std::move(discovery);
}

[[nodiscard]] std::expected<ManifestData, ManifestError> load_package_manifest(const std::string& manifest_path) {
    auto manifest_result = parse_manifest(manifest_path);
    if (!manifest_result.has_value()) return std::unexpected(std::move(manifest_result).error());
    ManifestData manifest = std::move(manifest_result).value();
    if (!manifest.package_name.has_value()) {
        return std::unexpected(ManifestError{"dependency manifest '" + manifest.manifest_path + "' does not declare [package]"});
    }
    return std::move(manifest);
}

[[nodiscard]] std::expected<WorkspaceInfo, ManifestError> load_workspace(const std::string& workspace_manifest_path) {
    auto ws_result = parse_manifest(workspace_manifest_path);
    if (!ws_result.has_value()) return std::unexpected(std::move(ws_result).error());
    ManifestData workspace_manifest = std::move(ws_result).value();
    WorkspaceInfo workspace{};
    std::string root_dir = path_parent(workspace_manifest.manifest_path);
    StringMap<std::string> seen_names{};

    if (workspace_manifest.package_name.has_value()) {
        auto self_result = parse_manifest(workspace_manifest.manifest_path);
        if (!self_result.has_value()) return std::unexpected(std::move(self_result).error());
        ManifestData self_manifest = std::move(self_result).value();
        seen_names.emplace(*self_manifest.package_name, self_manifest.manifest_path);
        workspace.member_manifests.push_back(std::move(self_manifest));
    }

    for (std::size_t i = 0; i < workspace_manifest.workspace->members.size(); i++) {
        const std::string& member_path = workspace_manifest.workspace->members[i];
        std::string member_manifest_path = normalized_path(path_join(path_join(root_dir, member_path), "scpp.toml"));
        if (!path_exists(member_manifest_path)) {
            return std::unexpected(ManifestError{"workspace member '" + path_join(root_dir, member_path) + "' has no scpp.toml"});
        }
        auto member_manifest_result = load_package_manifest(member_manifest_path);
        if (!member_manifest_result.has_value()) return std::unexpected(std::move(member_manifest_result).error());
        ManifestData member_manifest = std::move(member_manifest_result).value();
        if (member_manifest.manifest_path == workspace_manifest.manifest_path) continue;
        if (member_manifest.workspace.has_value()) {
            return std::unexpected(ManifestError{"nested workspace member '" + member_manifest.manifest_path +
                                "' is not supported"});
        }
        if (!member_manifest.package_name.has_value()) {
            return std::unexpected(ManifestError{"workspace member '" + member_manifest.manifest_path + "' must declare [package]"});
        }
        if (seen_names.contains(*member_manifest.package_name)) {
            return std::unexpected(ManifestError{"workspace contains duplicate package name '" + *member_manifest.package_name + "'"});
        }
        seen_names.emplace(*member_manifest.package_name, member_manifest.manifest_path);
        workspace.member_manifests.push_back(std::move(member_manifest));
    }

    if (workspace_manifest.workspace->has_default_members) {
        for (std::size_t i = 0; i < workspace_manifest.workspace->default_members.size(); i++) {
            const std::string& relative_path = workspace_manifest.workspace->default_members[i];
            std::string candidate = normalized_path(path_join(path_join(root_dir, relative_path), "scpp.toml"));
            if (relative_path == "." || relative_path.empty()) candidate = workspace_manifest.manifest_path;
            bool found = false;
            for (std::size_t j = 0; j < workspace.member_manifests.size(); j++) {
                if (workspace.member_manifests[j].manifest_path == candidate) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return std::unexpected(ManifestError{"default workspace member '" + relative_path + "' is not a declared workspace package"});
            }
            workspace.default_package_manifests.push_back(std::move(candidate));
        }
    } else if (workspace_manifest.package_name.has_value()) {
        workspace.default_package_manifests.push_back(workspace_manifest.manifest_path);
    } else {
        for (std::size_t i = 0; i < workspace.member_manifests.size(); i++) {
            workspace.default_package_manifests.push_back(workspace.member_manifests[i].manifest_path);
        }
    }
    workspace.manifest = std::move(workspace_manifest);
    return std::move(workspace);
}

std::string package_output_root(const std::string& shared_root_dir,
                               std::string_view package_name) {
    return path_join(path_join(path_join(shared_root_dir, ".scpp"), "build"),
                     path_join(scpp::host_target_triple(), string_from_view(package_name)));
}

[[nodiscard]] std::expected<void, BuildError> write_metadata_file(
    const PackageBuildResult& result, const ManifestData& manifest, const std::string& metadata_path) {
    std::string json{};
    json += "{\n";
    json += "  \"package\": \"" + escape_json(*manifest.package_name) + "\",\n";
    json += "  \"triple\": \"" + escape_json(scpp::host_target_triple()) + "\",\n";
    json += "  \"modules\": [\n";
    for (std::size_t i = 0; i < result.library_modules.size(); i++) {
        const BuiltModule& mod = result.library_modules[i];
        json += "    {\"name\": \"" + escape_json(mod.name) + "\", \"interface\": \""
             + escape_json(mod.interface_path) + "\", \"archive\": \""
             + escape_json(mod.archive_path) + "\"}";
        if (i + 1 < result.library_modules.size()) json += ",";
        json += "\n";
    }
    json += "  ],\n";
    json += "  \"binaries\": [\n";
    for (std::size_t i = 0; i < result.binaries.size(); i++) {
        json += "    \"" + escape_json(result.binaries[i]) + "\"";
        if (i + 1 < result.binaries.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    json += "}\n";
    return write_file(metadata_path, json);
}

class PackageBuilder {
public:
    PackageBuilder(std::string shared_root_dir)
        : shared_root_dir_{normalized_path(shared_root_dir)},
          database_{path_join(path_join(path_join(normalized_path(shared_root_dir), ".scpp"), "cache"), "build.db")} {}

    virtual ~PackageBuilder() = default;

    [[nodiscard]] std::expected<void, BuildError> open_database() { return database_.open(); }

    [[nodiscard]] std::expected<std::reference_wrapper<PackageBuildResult>, BuildError> build_package(
        const std::string& manifest_path, bool build_binaries, const scpp::ProjectBuildOptions& options) {
        std::string normalized_manifest = normalized_path(manifest_path);
        std::string key = normalized_manifest;

        if (cache_.contains(key)) {
            PackageBuildResult& cached = cache_.at(key);
            bool has_all_libraries =
                cached.library_modules.size() >= cached.manifest.lib_targets.size();
            if (has_all_libraries) {
                if (!build_binaries) {
                    return std::reference_wrapper<PackageBuildResult>{cached};
                }
                std::string selected_binary_path{};
                if (options.selected_bin.has_value()) {
                    selected_binary_path = path_join(cached.package_output_root, *options.selected_bin);
                }
                bool has_requested_binary = options.selected_bin.has_value()
                    ? vector_contains(cached.binaries, selected_binary_path)
                    : (!cached.binaries.empty() || cached.manifest.bin_targets.empty());
                if (has_requested_binary) {
                    return std::reference_wrapper<PackageBuildResult>{cached};
                }
            }
        }

        if (!recursion_stack_.insert(key).second) {
            return std::unexpected(BuildError{"cyclic package dependency involving '" + normalized_manifest + "'"});
        }

        trace_build("build package " + normalized_manifest);
        auto manifest_result = load_package_manifest(normalized_manifest);
        if (!manifest_result.has_value()) {
            recursion_stack_.erase(key);
            return std::unexpected(BuildError{manifest_result.error().what()});
        }
        ManifestData manifest = std::move(manifest_result).value();
        PackageBuildResult result{};
        result.package_output_root = package_output_root(shared_root_dir_, *manifest.package_name);
        result.native_link_inputs = expand_native_link_inputs(manifest);

        std::string modules_dir = path_join(result.package_output_root, "modules");
        std::string archives_dir = path_join(result.package_output_root, "archives");
        std::string objects_dir = path_join(result.package_output_root, "objects");
        if (!path_create_directories(modules_dir) ||
            !path_create_directories(archives_dir) ||
            !path_create_directories(objects_dir)) {
            recursion_stack_.erase(key);
            return std::unexpected(BuildError{"failed to create package output directories under '" +
                                    result.package_output_root + "'"});
        }

        StringMap<std::string> direct_import_paths{};
        StringMap<std::string> full_dependency_import_paths{};
        StringMap<std::string> direct_module_owners{};
        StringMap<std::string> transitive_only_modules{};

        for (std::size_t i = 0; i < manifest.dependencies.size(); i++) {
            const DependencySpec& dep = manifest.dependencies[i];
            std::string dep_manifest_path =
                normalized_path(path_join(path_join(path_parent(manifest.manifest_path), dep.path), "scpp.toml"));
            if (!path_exists(dep_manifest_path)) {
                recursion_stack_.erase(key);
                return std::unexpected(BuildError{"dependency '" + dep.alias + "' path '" +
                                 path_join(path_parent(manifest.manifest_path), dep.path) + "' has no scpp.toml"});
            }
            auto dep_result_or_error = build_package(dep_manifest_path, /*build_binaries=*/false, scpp::ProjectBuildOptions{});
            if (!dep_result_or_error.has_value()) {
                recursion_stack_.erase(key);
                return std::unexpected(std::move(dep_result_or_error).error());
            }
            PackageBuildResult& dep_result = dep_result_or_error.value().get();
            if (dep_result.library_modules.empty()) {
                recursion_stack_.erase(key);
                return std::unexpected(BuildError{"dependency package '" + *dep_result.manifest.package_name +
                                 "' does not provide a [[lib]] target"});
            }
            for (std::size_t j = 0; j < dep_result.exported_modules.size(); j++) {
                const std::string& module_name = dep_result.exported_modules.keys_[j];
                const std::string& interface_path = dep_result.exported_modules.values_[j];
                if (direct_import_paths.contains(module_name)) {
                    if (direct_import_paths.at(module_name) != interface_path) {
                        recursion_stack_.erase(key);
                        return std::unexpected(BuildError{"module '" + module_name + "' is exported by multiple direct dependencies"});
                    }
                } else {
                    direct_import_paths.emplace(module_name, interface_path);
                }
                direct_module_owners.emplace(module_name, *dep_result.manifest.package_name);
            }
            if (auto r = append_import_maps(full_dependency_import_paths, dep_result.exported_modules); !r.has_value()) {
                recursion_stack_.erase(key);
                return std::unexpected(std::move(r).error());
            }
            if (auto r = append_import_maps(full_dependency_import_paths, dep_result.closure_import_paths); !r.has_value()) {
                recursion_stack_.erase(key);
                return std::unexpected(std::move(r).error());
            }
            for (std::size_t j = 0; j < dep_result.closure_module_owners.size(); j++) {
                const std::string& module_name = dep_result.closure_module_owners.keys_[j];
                const std::string& owner = dep_result.closure_module_owners.values_[j];
                if (!direct_import_paths.contains(module_name) && !transitive_only_modules.contains(module_name)) {
                    transitive_only_modules.emplace(module_name, owner);
                }
            }
            for (std::size_t j = 0; j < dep_result.library_modules.size(); j++) {
                result.archive_closure.push_back(dep_result.library_modules[j].archive_path);
            }
            append_unique_paths(result.archive_closure, dep_result.archive_closure);
            append_unique_strings(result.native_link_inputs, dep_result.native_link_inputs);
            result.uses_stdlib = result.uses_stdlib || dep_result.uses_stdlib;
        }

        std::vector<std::string> referenced_custom_steps{};
        if (options.selected_lib.has_value()) {
            for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
                const ManifestTarget& target = manifest.lib_targets[i];
                if (target.name == *options.selected_lib) {
                    append_unique_strings(referenced_custom_steps, target.additional_obj_steps);
                    break;
                }
            }
        } else {
            for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
                append_unique_strings(referenced_custom_steps, manifest.lib_targets[i].additional_obj_steps);
            }
        }
        if (build_binaries) {
            if (options.selected_bin.has_value()) {
                for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
                    const ManifestTarget& target = manifest.bin_targets[i];
                    if (target.name == *options.selected_bin) {
                        append_unique_strings(referenced_custom_steps, target.additional_obj_steps);
                        break;
                    }
                }
            } else {
                for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
                    append_unique_strings(referenced_custom_steps, manifest.bin_targets[i].additional_obj_steps);
                }
            }
        }
        for (std::size_t i = 0; i < referenced_custom_steps.size(); i++) {
            const std::string& step_name = referenced_custom_steps[i];
            auto outputs_result = build_custom_step_with_cache(manifest, result.package_output_root, step_name);
            if (!outputs_result.has_value()) {
                recursion_stack_.erase(key);
                return std::unexpected(std::move(outputs_result).error());
            }
            result.custom_outputs.emplace(step_name, std::move(outputs_result).value());
        }

        if (options.selected_lib.has_value()) {
            bool found_target = false;
            for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
                const ManifestTarget& target = manifest.lib_targets[i];
                if (target.name == *options.selected_lib) {
                    found_target = true;
                    if (auto r = build_library_target(manifest, target, direct_import_paths, full_dependency_import_paths,
                                         transitive_only_modules, result); !r.has_value()) {
                        recursion_stack_.erase(key);
                        return std::unexpected(std::move(r).error());
                    }
                    break;
                }
            }
            if (!found_target) {
                recursion_stack_.erase(key);
                return std::unexpected(BuildError{"unknown [[lib]] target '" + *options.selected_lib + "'"});
            }
        } else {
            for (std::size_t i = 0; i < manifest.lib_targets.size(); i++) {
                if (auto r = build_library_target(manifest, manifest.lib_targets[i], direct_import_paths, full_dependency_import_paths,
                                     transitive_only_modules, result); !r.has_value()) {
                    recursion_stack_.erase(key);
                    return std::unexpected(std::move(r).error());
                }
            }
        }
        if (manifest.lib_targets.empty() && options.build_lib_only) {
            recursion_stack_.erase(key);
            return std::unexpected(BuildError{"manifest has no [[lib]] target"});
        }

        result.exported_modules = to_import_map(result.library_modules);
        result.closure_import_paths = result.exported_modules;
        if (auto r = append_import_maps(result.closure_import_paths, full_dependency_import_paths); !r.has_value()) {
            recursion_stack_.erase(key);
            return std::unexpected(std::move(r).error());
        }
        for (std::size_t i = 0; i < result.library_modules.size(); i++) {
            result.closure_module_owners.emplace(result.library_modules[i].name, *manifest.package_name);
        }
        for (std::size_t i = 0; i < direct_module_owners.size(); i++) {
            const std::string& module_name = direct_module_owners.keys_[i];
            const std::string& owner = direct_module_owners.values_[i];
            if (!result.closure_module_owners.contains(module_name)) {
                result.closure_module_owners.emplace(module_name, owner);
            }
        }
        for (std::size_t i = 0; i < transitive_only_modules.size(); i++) {
            const std::string& module_name = transitive_only_modules.keys_[i];
            const std::string& owner = transitive_only_modules.values_[i];
            if (!result.closure_module_owners.contains(module_name)) {
                result.closure_module_owners.emplace(module_name, owner);
            }
        }

        if (build_binaries) {
            if (options.selected_bin.has_value()) {
                bool found_target = false;
                for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
                    const ManifestTarget& target = manifest.bin_targets[i];
                    if (target.name == *options.selected_bin) {
                        found_target = true;
                        if (auto r = build_binary_target(manifest, target, direct_import_paths, full_dependency_import_paths,
                                            transitive_only_modules, result); !r.has_value()) {
                            recursion_stack_.erase(key);
                            return std::unexpected(std::move(r).error());
                        }
                        break;
                    }
                }
                if (!found_target) {
                    recursion_stack_.erase(key);
                    return std::unexpected(BuildError{"unknown [[bin]] target '" + *options.selected_bin + "'"});
                }
            } else {
                for (std::size_t i = 0; i < manifest.bin_targets.size(); i++) {
                    if (auto r = build_binary_target(manifest, manifest.bin_targets[i], direct_import_paths, full_dependency_import_paths,
                                        transitive_only_modules, result); !r.has_value()) {
                        recursion_stack_.erase(key);
                        return std::unexpected(std::move(r).error());
                    }
                }
            }
        }

        std::string metadata_path = path_join(result.package_output_root, "package-metadata.json");
        if (auto r = write_metadata_file(result, manifest, metadata_path); !r.has_value()) {
            recursion_stack_.erase(key);
            return std::unexpected(std::move(r).error());
        }
        std::vector<std::string> pkg_sig_items{};
        pkg_sig_items.push_back("manifest=" + manifest_digest(manifest));
        pkg_sig_items.push_back("triple=" + scpp::host_target_triple());
        pkg_sig_items.push_back("metadata=" + digest_file(metadata_path));
        std::string pkg_sig = fnv1a64_hex(join_for_digest(pkg_sig_items));
        if (auto r = database_.put(BuildRecord{
                "package|" + manifest.manifest_path,
                "package",
                pkg_sig,
                {},
                {},
                digest_file(metadata_path),
                metadata_path,
                manifest_digest(manifest),
                compiler_version_key(),
                scpp::host_target_triple()
            });
            !r.has_value()) {
            recursion_stack_.erase(key);
            return std::unexpected(std::move(r).error());
        }

        result.manifest = std::move(manifest);
        cache_.emplace(key, std::move(result));
        recursion_stack_.erase(key);
        return std::reference_wrapper<PackageBuildResult>{cache_.at(key)};
    }

private:
    [[nodiscard]] std::expected<std::vector<std::string>, BuildError> build_custom_step_with_cache(
        const ManifestData& manifest, const std::string& package_output_root_dir, const std::string& step_name) {
        if (!manifest.custom_commands.contains(step_name)) {
            return std::unexpected(BuildError{"unknown additional_objs step '" + step_name + "' in '" + manifest.manifest_path + "'"});
        }
        const CustomCommand& step = manifest.custom_commands.at(step_name);
        std::string manifest_dir = path_parent(manifest.manifest_path);
        std::string work_dir = path_join(path_join(package_output_root_dir, "custom"), sanitize_filename(step_name));

        std::vector<std::string> inputs{};
        inputs.reserve(step.input_paths.size());
        for (std::size_t i = 0; i < step.input_paths.size(); i++) {
            const std::string& input = step.input_paths[i];
            std::string resolved = (!input.empty() && input.at(0) == '/') ? normalized_path(input) : normalized_path(path_join(manifest_dir, input));
            if (!path_exists(resolved)) {
                return std::unexpected(BuildError{"[additional_objs." + step_name + "] input '" + resolved + "' does not exist"});
            }
            inputs.push_back(std::move(resolved));
        }

        std::vector<std::string> outputs{};
        outputs.reserve(step.output_paths.size());
        for (std::size_t i = 0; i < step.output_paths.size(); i++) {
            const std::string& output = step.output_paths[i];
            std::string resolved = (!output.empty() && output.at(0) == '/') ? path_lexically_normal(output) : path_lexically_normal(path_join(work_dir, output));
            outputs.push_back(std::move(resolved));
        }

        std::vector<std::string> input_keys = path_digests(inputs);
        sort_vector(input_keys);
        std::vector<std::string> output_keys{};
        output_keys.reserve(outputs.size());
        for (std::size_t i = 0; i < outputs.size(); i++) output_keys.push_back(outputs[i]);
        sort_vector(output_keys);

        std::vector<std::string> custom_sig_items{};
        custom_sig_items.push_back("kind=custom");
        custom_sig_items.push_back("manifest=" + manifest_digest(manifest));
        custom_sig_items.push_back("compiler=" + compiler_version_key());
        custom_sig_items.push_back("command=" + step.command);
        custom_sig_items.push_back("inputs=" + join_for_digest(input_keys));
        custom_sig_items.push_back("outputs=" + join_for_digest(output_keys));
        std::string signature = fnv1a64_hex(join_for_digest(custom_sig_items));
        std::string record_key = "custom|" + manifest.manifest_path + "|" + step_name;

        bool outputs_exist = true;
        for (std::size_t i = 0; i < outputs.size(); i++) {
            if (!path_exists(outputs[i])) {
                outputs_exist = false;
                break;
            }
        }
        auto cached_result = database_.get(record_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result.value().has_value() && cached_result.value().value().signature == signature && outputs_exist) {
            trace_build("cache hit custom " + step_name);
            return outputs;
        }

        if (auto r = prepare_custom_workdir(manifest_dir, work_dir); !r.has_value()) return std::unexpected(std::move(r).error());
        for (std::size_t i = 0; i < outputs.size(); i++) {
            std::string parent = path_parent(outputs[i]);
            if (!parent.empty() && parent != "." && parent != "/") {
                if (!path_create_directories(parent)) {
                    return std::unexpected(BuildError{"failed to create directory '" + parent + "'"});
                }
            }
        }

        trace_build("run custom " + step_name);
        std::string command = "cd " + quote_for_shell(work_dir) + " && ";
        const char* cxx_env = nullptr;
        char cxx_first = '\0';
        [[scpp::unsafe]] {
            cxx_env = getenv("CXX");
            if (cxx_env != nullptr) cxx_first = cxx_env[0];
        }
        if (cxx_env == nullptr || cxx_first == '\0') {
            if (std::optional<std::string> cxx = default_custom_step_cxx(); cxx.has_value()) {
                command += "export CXX=" + quote_for_shell(*cxx) + " && ";
            }
        }
        command += step.command;
        int result = -1;
        [[scpp::unsafe]] {
            result = system(command.c_str());
        }
        if (result != 0) {
            return std::unexpected(BuildError{"[additional_objs." + step_name + "] command failed: " + command});
        }
        for (std::size_t i = 0; i < outputs.size(); i++) {
            if (!path_exists(outputs[i])) {
                return std::unexpected(BuildError{"[additional_objs." + step_name + "] did not produce expected output '" + outputs[i] + "'"});
            }
        }

        std::vector<std::string> output_digests = path_digests(outputs);
        sort_vector(output_digests);
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
            scpp::host_target_triple()
        }); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }
        return outputs;
    }

    [[nodiscard]] std::expected<std::vector<std::string>, BuildError> collect_custom_outputs(
        const PackageBuildResult& result, const ManifestTarget& target) const {
        std::vector<std::string> outputs{};
        for (std::size_t i = 0; i < target.additional_obj_steps.size(); i++) {
            const std::string& step_name = target.additional_obj_steps[i];
            if (!result.custom_outputs.contains(step_name)) {
                return std::unexpected(BuildError{"custom step '" + step_name + "' was not built for target '" + target.name + "'"});
            }
            append_unique_paths(outputs, result.custom_outputs.at(step_name));
        }
        return outputs;
    }

    [[nodiscard]] std::expected<void, BuildError> build_library_target(
        const ManifestData& manifest, const ManifestTarget& lib_target,
        const StringMap<std::string>& direct_dep_import_paths,
        const StringMap<std::string>& full_dependency_import_paths,
        const StringMap<std::string>& transitive_only_modules, PackageBuildResult& result) {
        std::string manifest_dir = path_parent(manifest.manifest_path);
        auto lib_sources_result = classify_target_sources(manifest_dir, lib_target);
        if (!lib_sources_result.has_value()) return std::unexpected(std::move(lib_sources_result).error());
        std::vector<SourceInfo> lib_sources = std::move(lib_sources_result).value();
        StringSet own_library_module_names = local_primary_module_names(lib_sources);
        if (auto r = validate_direct_visibility(lib_sources, own_library_module_names, direct_dep_import_paths,
                                                 transitive_only_modules);
            !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }
        result.uses_stdlib = result.uses_stdlib || sources_use_stdlib(lib_sources);
        auto built_lib_modules_result = build_modules_for_target(
            lib_sources, path_join(result.package_output_root, "modules"), path_join(result.package_output_root, "archives"),
            full_dependency_import_paths, kManifestBuildOptLevel, manifest, &lib_target, database_);
        if (!built_lib_modules_result.has_value()) return std::unexpected(std::move(built_lib_modules_result).error());
        std::vector<BuiltModule> built_lib_modules = std::move(built_lib_modules_result).value();
        auto lib_custom_outputs_result = collect_custom_outputs(result, lib_target);
        if (!lib_custom_outputs_result.has_value()) return std::unexpected(std::move(lib_custom_outputs_result).error());
        std::vector<std::string> lib_custom_outputs = std::move(lib_custom_outputs_result).value();
        if (!lib_custom_outputs.empty()) {
            std::vector<std::string> archive_inputs{};
            for (std::size_t i = 0; i < lib_custom_outputs.size(); i++) archive_inputs.push_back(lib_custom_outputs[i]);
            auto archive_r = scpp::archive_objects(archive_inputs, built_lib_modules[0].archive_path);
            if (!archive_r.has_value()) {
                return std::unexpected(BuildError{archive_r.error().what()});
            }
            built_lib_modules[0].archive_digest = path_digest_or_empty(built_lib_modules[0].archive_path);
        }
        for (std::size_t i = 0; i < built_lib_modules.size(); i++) {
            result.library_modules.push_back(std::move(built_lib_modules[i]));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, BuildError> build_binary_target(
        const ManifestData& manifest, const ManifestTarget& bin_target,
        const StringMap<std::string>& direct_dep_import_paths,
        const StringMap<std::string>& full_dependency_import_paths,
        const StringMap<std::string>& transitive_only_modules, PackageBuildResult& result) {
        std::string manifest_dir = path_parent(manifest.manifest_path);
        auto sources_result = classify_target_sources(manifest_dir, bin_target);
        if (!sources_result.has_value()) return std::unexpected(std::move(sources_result).error());
        std::vector<SourceInfo> sources = std::move(sources_result).value();

        StringMap<std::string> base_import_paths = full_dependency_import_paths;
        if (auto r = append_import_maps(base_import_paths, result.exported_modules); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

        StringSet local_modules = own_module_names(result.library_modules);
        StringSet bin_local_names = local_primary_module_names(sources);
        for (std::size_t i = 0; i < bin_local_names.elements_.size(); i++) {
            local_modules.insert(bin_local_names.elements_[i]);
        }
        if (auto r = validate_direct_visibility(sources, local_modules, direct_dep_import_paths, transitive_only_modules);
            !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

        StringSet library_source_paths{};
        for (std::size_t i = 0; i < result.library_modules.size(); i++) {
            library_source_paths.insert(result.library_modules[i].source_path);
        }
        std::vector<SourceInfo> local_module_sources{};
        local_module_sources.reserve(sources.size());
        for (std::size_t i = 0; i < sources.size(); i++) {
            const SourceInfo& source = sources[i];
            if ((source.kind == SourceInfo::Kind::PrimaryInterface || source.kind == SourceInfo::Kind::InterfacePartition ||
                 source.kind == SourceInfo::Kind::ImplementationPartition) &&
                library_source_paths.contains(source.path)) {
                continue;
            }
            local_module_sources.push_back(source);
        }

        std::string module_dir = path_join(result.package_output_root, "modules");
        std::string archive_dir = path_join(result.package_output_root, "archives");
        std::string object_dir = path_join(path_join(result.package_output_root, "objects"), sanitize_filename(bin_target.name));
        if (!path_create_directories(object_dir)) {
            return std::unexpected(BuildError{"failed to create directory '" + object_dir + "'"});
        }

        auto local_modules_built_result =
            build_modules_for_target(local_module_sources, module_dir, archive_dir, base_import_paths,
                                     kManifestBuildOptLevel, manifest, nullptr, database_);
        if (!local_modules_built_result.has_value()) return std::unexpected(std::move(local_modules_built_result).error());
        std::vector<BuiltModule> local_modules_built = std::move(local_modules_built_result).value();
        StringMap<std::string> compile_import_paths = base_import_paths;
        if (auto r = append_import_maps(compile_import_paths, to_import_map(local_modules_built)); !r.has_value()) {
            return std::unexpected(std::move(r).error());
        }

        bool binary_uses_stdlib = result.uses_stdlib || sources_use_stdlib(sources);

        std::vector<std::string> plain_objects{};
        std::size_t plain_index = 0;
        for (std::size_t i = 0; i < sources.size(); i++) {
            const SourceInfo& source = sources[i];
            if (source.kind != SourceInfo::Kind::Plain) continue;
            std::string object_path = path_join(object_dir, std::to_string(plain_index++) + "_" +
                                                              sanitize_filename(path_filename(source.path)) + ".o");
            if (auto r = build_object_with_cache("plain:" + bin_target.name + ":" + std::to_string(plain_index - 1), manifest,
                                                  source, object_path, compile_import_paths);
                !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
            plain_objects.push_back(object_path);
        }

        std::vector<std::string> extra_link_inputs{};
        auto bin_custom_outputs_result = collect_custom_outputs(result, bin_target);
        if (!bin_custom_outputs_result.has_value()) return std::unexpected(std::move(bin_custom_outputs_result).error());
        for (std::size_t i = 0; i < bin_custom_outputs_result.value().size(); i++) {
            extra_link_inputs.push_back(bin_custom_outputs_result.value()[i]);
        }
        for (std::size_t i = 0; i < plain_objects.size(); i++) extra_link_inputs.push_back(plain_objects[i]);
        for (std::size_t i = local_modules_built.size(); i > 0; i--) {
            extra_link_inputs.push_back(local_modules_built[i - 1].archive_path);
        }
        for (std::size_t i = result.library_modules.size(); i > 0; i--) {
            extra_link_inputs.push_back(result.library_modules[i - 1].archive_path);
        }
        for (std::size_t i = 0; i < result.archive_closure.size(); i++) extra_link_inputs.push_back(result.archive_closure[i]);
        if (binary_uses_stdlib) append_unique_strings(extra_link_inputs, scpp::project_default_stdlib_link_inputs());
        append_unique_strings(extra_link_inputs, result.native_link_inputs);

        std::string executable_path = path_join(result.package_output_root, bin_target.name);
        std::string binary_key = "binary|" + manifest.manifest_path + "|" + bin_target.name;
        std::vector<std::string> binary_inputs{};
        binary_inputs.push_back("manifest=" + manifest_digest(manifest));
        binary_inputs.push_back("triple=" + scpp::host_target_triple());
        binary_inputs.push_back("compiler=" + compiler_version_key());
        for (std::size_t i = 0; i < plain_objects.size(); i++) {
            binary_inputs.push_back("obj=" + plain_objects[i] + "#" + path_digest_or_empty(plain_objects[i]));
        }
        for (std::size_t i = 0; i < extra_link_inputs.size(); i++) {
            const std::string& input = extra_link_inputs[i];
            if (!input.empty() && input[0] == '-') {
                binary_inputs.push_back("flag=" + input);
            } else {
                binary_inputs.push_back("link=" + input + "#" + path_digest_or_empty(input));
            }
        }
        sort_vector(binary_inputs);
        std::string link_signature = fnv1a64_hex(join_for_digest(binary_inputs));
        auto cached_result = database_.get(binary_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result.value().has_value() && cached_result.value().value().signature == link_signature &&
            path_exists(executable_path)) {
            trace_build("cache hit link " + executable_path);
        } else {
            trace_build("link binary " + executable_path);
            auto link_r = scpp::link_executable(extra_link_inputs, executable_path, /*static_link=*/true);
            if (!link_r.has_value()) {
                return std::unexpected(BuildError{link_r.error().what()});
            }
            if (auto r = database_.put(BuildRecord{
                binary_key,
                "binary",
                link_signature,
                {},
                {},
                path_digest_or_empty(executable_path),
                executable_path,
                manifest_digest(manifest),
                compiler_version_key(),
                scpp::host_target_triple()
            }); !r.has_value()) {
                return std::unexpected(std::move(r).error());
            }
        }
        if (!vector_contains(result.binaries, executable_path)) {
            result.binaries.push_back(executable_path);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, BuildError> build_object_with_cache(
        const std::string& key_suffix, const ManifestData& manifest, const SourceInfo& source,
        const std::string& object_path, const StringMap<std::string>& import_paths) {
        std::vector<std::string> dep_keys{};
        for (std::size_t i = 0; i < source.imported_modules.size(); i++) {
            const std::string& imported = source.imported_modules[i];
            if (!import_paths.contains(imported)) continue;
            dep_keys.push_back(imported + "=" + import_paths.at(imported) + "#" + path_digest_or_empty(import_paths.at(imported)));
        }
        sort_vector(dep_keys);
        std::vector<std::string> obj_sig_items{};
        obj_sig_items.push_back("kind=object");
        obj_sig_items.push_back("source=" + digest_file(source.path));
        obj_sig_items.push_back("triple=" + scpp::host_target_triple());
        obj_sig_items.push_back("compiler=" + compiler_version_key());
        obj_sig_items.push_back("manifest=" + manifest_digest(manifest));
        obj_sig_items.push_back("opt=" + std::to_string(static_cast<std::int64_t>(kManifestBuildOptLevel)));
        obj_sig_items.push_back("debug=" + std::string{kManifestBuildEmitDebugInfo ? "1" : "0"});
        obj_sig_items.push_back("deps=" + join_for_digest(dep_keys));
        std::string signature = fnv1a64_hex(join_for_digest(obj_sig_items));
        std::string record_key = "object|" + manifest.manifest_path + "|" + key_suffix;
        auto cached_result = database_.get(record_key);
        if (!cached_result.has_value()) return std::unexpected(std::move(cached_result).error());
        if (cached_result.value().has_value() && cached_result.value().value().signature == signature && path_exists(object_path)) {
            trace_build("cache hit object " + object_path);
            return {};
        }
        trace_build("build object " + object_path);
        auto source_text_result = read_file(source.path);
        if (!source_text_result.has_value()) return std::unexpected(std::move(source_text_result).error());
        std::string source_text = std::move(source_text_result).value();
        auto emit_r = scpp::emit_object_file(source_text, object_path, to_std_map(import_paths), {}, kManifestBuildEmitDebugInfo,
                               source.path, kManifestBuildOptLevel);
        if (!emit_r.has_value()) {
            print_diagnostic(source.path, source_text, emit_r.error().loc, emit_r.error().what());
            return std::unexpected(BuildError{emit_r.error().what()});
        }
        return database_.put(BuildRecord{
            record_key,
            "object",
            signature,
            {},
            {},
            path_digest_or_empty(object_path),
            object_path,
            manifest_digest(manifest),
            compiler_version_key(),
            scpp::host_target_triple()
        });
    }

    StringSet own_module_names(const std::vector<BuiltModule>& modules) const {
        StringSet names{};
        for (std::size_t i = 0; i < modules.size(); i++) names.insert(modules[i].name);
        return names;
    }

    std::string shared_root_dir_{};
    StringMap<PackageBuildResult> cache_{};
    StringSet recursion_stack_{};
    BuildDatabase database_;
};

[[nodiscard]] std::expected<std::vector<std::string>, ManifestError> select_workspace_packages(
    const WorkspaceInfo& workspace, const ManifestData* current_manifest, const scpp::ProjectBuildOptions& options,
    bool invoked_from_workspace_root) {
    if (options.build_workspace && options.selected_package.has_value()) {
        return std::unexpected(ManifestError{"--workspace and --package/-p cannot be used together"});
    }
    if (options.selected_bin.has_value() && options.build_workspace) {
        return std::unexpected(ManifestError{"--bin cannot be combined with --workspace"});
    }
    if (options.selected_lib.has_value() && options.build_workspace) {
        return std::unexpected(ManifestError{"--lib <name> cannot be combined with --workspace"});
    }

    auto find_by_name = [&](const std::string& name) -> std::optional<std::string> {
        for (std::size_t i = 0; i < workspace.member_manifests.size(); i++) {
            const ManifestData& manifest = workspace.member_manifests[i];
            if (manifest.package_name.has_value() && *manifest.package_name == name) return manifest.manifest_path;
        }
        return std::nullopt;
    };

    if (options.selected_package.has_value()) {
        std::optional<std::string> selected = find_by_name(*options.selected_package);
        if (!selected.has_value()) {
            return std::unexpected(ManifestError{"workspace has no package named '" + *options.selected_package + "'"});
        }
        std::vector<std::string> res{};
        res.push_back(selected.value());
        return res;
    }
    if (options.build_workspace) {
        std::vector<std::string> res{};
        for (std::size_t i = 0; i < workspace.member_manifests.size(); i++) {
            res.push_back(workspace.member_manifests[i].manifest_path);
        }
        return res;
    }
    if (!invoked_from_workspace_root && current_manifest != nullptr) {
        std::optional<std::string> pkg_name{};
        std::string manifest_path{};
        [[scpp::unsafe]] {
            pkg_name = current_manifest->package_name;
            manifest_path = current_manifest->manifest_path;
        }
        if (pkg_name.has_value()) {
            std::vector<std::string> res{};
            res.push_back(manifest_path);
            return res;
        }
    }

    std::vector<std::string> selected{};
    for (std::size_t i = 0; i < workspace.default_package_manifests.size(); i++) {
        const std::string& manifest_path = workspace.default_package_manifests[i];
        bool found = false;
        for (std::size_t j = 0; j < workspace.member_manifests.size(); j++) {
            const ManifestData& manifest = workspace.member_manifests[j];
            if (manifest.manifest_path == manifest_path) {
                selected.push_back(manifest_path);
                found = true;
                break;
            }
        }
        if (!found) {
            return std::unexpected(ManifestError{"workspace default member '" + manifest_path + "' could not be resolved"});
        }
    }
    return selected;
}

} // namespace scpp

export namespace scpp {

std::optional<std::string> find_project_manifest(const std::string& start_dir) {
    std::vector<std::string> manifests = manifests_upward(start_dir);
    if (manifests.empty()) return std::nullopt;
    return manifests[0];
}

int build_manifest_project(const std::string& start_dir, const ProjectBuildOptions& options) {
    auto discovery_result = discover_project(start_dir);
    if (!discovery_result.has_value()) {
        eprintln("error: " + discovery_result.error().what());
        return 1;
    }
    ProjectDiscovery discovery = std::move(discovery_result).value();
    if (!discovery.current_manifest_path.has_value()) {
        eprintln("error: no scpp.toml found in the current directory or any parent directory");
        return 1;
    }
    auto current_manifest_result = parse_manifest(discovery.current_manifest_path.value());
    if (!current_manifest_result.has_value()) {
        eprintln("error: " + current_manifest_result.error().what());
        return 1;
    }
    ManifestData current_manifest = std::move(current_manifest_result).value();

    std::optional<WorkspaceInfo> workspace_info{};
    std::vector<std::string> manifests_to_build{};
    std::string shared_output_root{};
    bool invoked_from_workspace_root = false;

    if (discovery.workspace_manifest_path.has_value()) {
        auto candidate_workspace_result = load_workspace(discovery.workspace_manifest_path.value());
        if (!candidate_workspace_result.has_value()) {
            eprintln("error: " + candidate_workspace_result.error().what());
            return 1;
        }
        WorkspaceInfo candidate_workspace = std::move(candidate_workspace_result).value();
        bool current_is_workspace_root =
            current_manifest.manifest_path == discovery.workspace_manifest_path.value();
        bool current_is_declared_member = current_is_workspace_root;
        if (!current_is_declared_member) {
            for (std::size_t i = 0; i < candidate_workspace.member_manifests.size(); i++) {
                if (candidate_workspace.member_manifests[i].manifest_path == current_manifest.manifest_path) {
                    current_is_declared_member = true;
                    break;
                }
            }
        }
        if (current_is_declared_member) {
            workspace_info = std::move(candidate_workspace);
            shared_output_root = path_parent(discovery.workspace_manifest_path.value());
            invoked_from_workspace_root = current_is_workspace_root;
        }
    }

    if (workspace_info.has_value()) {
        if (!current_manifest.package_name.has_value() && !invoked_from_workspace_root) {
            eprintln("error: current manifest is not a package manifest");
            return 1;
        }
        auto packages_result = select_workspace_packages(*workspace_info,
                                                      current_manifest.package_name.has_value()
                                                          ? &current_manifest
                                                          : nullptr,
                                                      options, invoked_from_workspace_root);
        if (!packages_result.has_value()) {
            eprintln("error: " + packages_result.error().what());
            return 1;
        }
        for (std::size_t i = 0; i < packages_result.value().size(); i++) {
            manifests_to_build.push_back(packages_result.value()[i]);
        }
    } else {
        if (options.build_workspace || options.selected_package.has_value()) {
            eprintln("error: --workspace and --package/-p require a workspace root with [workspace]");
            return 1;
        }
        if (!current_manifest.package_name.has_value()) {
            eprintln("error: a virtual workspace cannot be built without [workspace] package selection");
            return 1;
        }
        shared_output_root = path_parent(current_manifest.manifest_path);
        manifests_to_build.push_back(current_manifest.manifest_path);
    }

    PackageBuilder builder{shared_output_root};
    if (auto r = builder.open_database(); !r.has_value()) {
        eprintln("error: " + r.error().what());
        return 1;
    }
    for (std::size_t i = 0; i < manifests_to_build.size(); i++) {
        std::string manifest_path = manifests_to_build[i];
        auto r = builder.build_package(manifest_path, /*build_binaries=*/!options.build_lib_only, options);
        if (!r.has_value()) {
            eprintln("error: " + r.error().what());
            return 1;
        }
    }
    return 0;
}

} // namespace scpp
