import scpp.driver;
import scpp.parser;
import scpp.compiler.movecheck;
import scpp.compiler.codegen;
import scpp.ast;
import std;

// `popen`/`pclose`/`FILE*` are POSIX extensions, not part of ISO C++'s
// standard library (they have no `import std;` module form), so `<cstdio>`
// stays as a real #include; same for `<sys/wait.h>` (POSIX `WIFEXITED` et
// al., used below).
#include <cstdio>
#include <sys/wait.h>

// SCPP_TEST_SOURCE_DIR and SCPP_DRIVER_TEST_SOURCE_DIR are injected by CMake
// (see the driver_test target in the top-level CMakeLists.txt) and point at
// tests/test_source and tests/driver_test_source respectively, so this binary
// finds its fixtures regardless of the working directory it's run from.
#ifndef SCPP_DRIVER_TEST_SOURCE_DIR
#error "SCPP_DRIVER_TEST_SOURCE_DIR must be defined by the build"
#endif
#ifndef SCPP_TEST_SOURCE_DIR
#error "SCPP_TEST_SOURCE_DIR must be defined by the build"
#endif
#ifndef SCPP_BINARY_PATH
#error "SCPP_BINARY_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_MODULE_PATH
#error "SCPP_STDLIB_STD_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_INTERFACE_PATH
#error "SCPP_STDLIB_STD_INTERFACE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_SCPP_MODULE_PATH
#error "SCPP_STDLIB_SCPP_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_SCPP_INTERFACE_PATH
#error "SCPP_STDLIB_SCPP_INTERFACE_PATH must be defined by the build"
#endif
namespace {

int failures = 0;
int cases_run = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        failures++;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<unsigned char> read_binary_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void write_u32_le(std::ostream& out, std::uint32_t value) {
    char bytes[4] = {static_cast<char>(value & 0xFFu), static_cast<char>((value >> 8) & 0xFFu),
                     static_cast<char>((value >> 16) & 0xFFu), static_cast<char>((value >> 24) & 0xFFu)};
    out.write(bytes, sizeof(bytes));
}

void write_legacy_scppm_without_payload(const std::filesystem::path& path, std::string_view interface_source) {
    std::ofstream out(path, std::ios::binary);
    const char header[8] = {'S', 'C', 'P', 'P', 'M', 1, 0, 0};
    out.write(header, sizeof(header));
    write_u32_le(out, static_cast<std::uint32_t>(interface_source.size()));
    out.write(interface_source.data(), static_cast<std::streamsize>(interface_source.size()));
}

std::unordered_map<std::string, std::string> source_module_import_paths() {
    return {{"std", SCPP_STDLIB_STD_MODULE_PATH}, {"scpp", SCPP_STDLIB_SCPP_MODULE_PATH}};
}

std::unordered_map<std::string, std::string> prebuilt_module_import_paths() {
    return {{"std", SCPP_STDLIB_STD_INTERFACE_PATH}, {"scpp", SCPP_STDLIB_SCPP_INTERFACE_PATH}};
}

std::vector<std::string> std_link_inputs() {
    return {};
}

class TestModuleCache {
public:
    explicit TestModuleCache(std::unordered_map<std::string, std::string> import_paths)
        : import_paths_(std::move(import_paths)) {}

    const scpp::Program& resolve(const std::string& module_name) {
        auto it = cache_.find(module_name);
        if (it != cache_.end()) return it->second;
        auto path_it = import_paths_.find(module_name);
        if (path_it == import_paths_.end()) throw std::runtime_error("unknown test module '" + module_name + "'");
        auto parsed_result = scpp::parse(
            read_file(path_it->second), [this](const std::string& name) -> const scpp::Program* { return &resolve(name); },
            [this](const std::string& key) -> scpp::Program { return resolve_partition(key); });
        if (!parsed_result.has_value()) throw std::move(parsed_result).error();
        auto [inserted, _] = cache_.emplace(module_name, std::move(parsed_result).value());
        return inserted->second;
    }

    scpp::Program resolve_partition(const std::string& key) {
        std::optional<std::string> path = infer_partition_path(key);
        if (!path.has_value()) throw std::runtime_error("unknown test partition '" + key + "'");
        auto result = scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program* { return &resolve(name); },
            [this](const std::string& nested_key) -> scpp::Program { return resolve_partition(nested_key); });
        if (!result.has_value()) throw std::move(result).error();
        return std::move(result).value();
    }

private:
    std::optional<std::string> infer_partition_path(const std::string& key) const {
        std::size_t colon = key.find(':');
        if (colon == std::string::npos) return std::nullopt;
        std::string module_name = key.substr(0, colon);
        auto module_it = import_paths_.find(module_name);
        if (module_it == import_paths_.end()) return std::nullopt;
        std::string partition_name = key.substr(colon + 1);
        std::filesystem::path module_path(module_it->second);
        std::filesystem::path candidate =
            module_path.parent_path() / partition_name /
            (module_path.stem().string() + "_" + partition_name + module_path.extension().string());
        if (!std::filesystem::exists(candidate)) return std::nullopt;
        return candidate.string();
    }

    std::unordered_map<std::string, std::string> import_paths_;
    std::unordered_map<std::string, scpp::Program> cache_;
};

std::expected<scpp::Program, scpp::ParseError> try_parse_with_std_imports(std::string_view source) {
    TestModuleCache cache(source_module_import_paths());
    return scpp::parse(
        source, [&cache](const std::string& name) -> const scpp::Program* { return &cache.resolve(name); },
        [&cache](const std::string& key) -> scpp::Program { return cache.resolve_partition(key); });
}

scpp::Program parse_program_with_std_imports(std::string_view source) {
    auto result = try_parse_with_std_imports(source);
    if (!result.has_value()) throw std::move(result).error();
    return std::move(result).value();
}

scpp::Program parse_with_std_imports(std::string_view source) {
    auto result = try_parse_with_std_imports(source);
    if (!result.has_value()) throw std::move(result).error();
    return std::move(result).value();
}

// Runs the full parse -> monomorphize_generics -> check_moves ->
// Codegen::generate pipeline and reports whether any stage failed. Used
// by tests below that only care "does this program fail to compile", not
// which specific stage rejected it -- every one of these tests' former
// catch-by-type cascades (ParseError/DataflowError/CodegenError) set the
// exact same `threw = true` regardless of which type was actually caught,
// so collapsing to a single bool here preserves their behavior exactly.
bool full_pipeline_fails(std::string_view source) {
    auto program_result = scpp::parse(source);
    if (!program_result.has_value()) return true;
    scpp::Program program = std::move(program_result.value());
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) return true;
    auto check_moves_result = scpp::check_moves(program);
    if (!check_moves_result.has_value()) return true;
    scpp::Codegen codegen("test_module");
    auto generate_result = codegen.generate(program);
    return !generate_result.has_value();
}

// Same as full_pipeline_fails, but for sources needing `import std;`/
// `import scpp;` module resolution.
bool full_pipeline_fails_with_std_imports(std::string_view source) {
    auto program_result = try_parse_with_std_imports(source);
    if (!program_result.has_value()) return true;
    scpp::Program program = std::move(program_result.value());
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) return true;
    auto check_moves_result = scpp::check_moves(program);
    if (!check_moves_result.has_value()) return true;
    scpp::Codegen codegen("test_module");
    auto generate_result = codegen.generate(program);
    return !generate_result.has_value();
}

// Like full_pipeline_fails, but stops after monomorphize_generics --
// used by tests exercising a rejection that monomorphize_generics itself
// is responsible for, where the original code never went on to call
// check_moves/codegen at all.
bool monomorphize_pipeline_fails(std::string_view source) {
    auto program_result = scpp::parse(source);
    if (!program_result.has_value()) return true;
    scpp::Program program = std::move(program_result.value());
    auto monomorphize_result = scpp::monomorphize_generics(program);
    return !monomorphize_result.has_value();
}

// Runs monomorphize_generics then check_moves over an already-parsed
// program and returns the DataflowError message from whichever stage
// failed first, or nullopt if both succeeded. Both stages report their
// failures as scpp::DataflowError, so the original code's single `catch
// (const scpp::DataflowError&)` covered either stage indiscriminately;
// this helper preserves that "either stage, same message check" behavior
// without needing a try/catch.
std::optional<std::string> monomorphize_or_check_moves_error_message(scpp::Program& program) {
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) return std::string(monomorphize_result.error().what());
    auto check_moves_result = scpp::check_moves(program);
    if (!check_moves_result.has_value()) return std::string(check_moves_result.error().what());
    return std::nullopt;
}

struct RunResult {
    int exit_code;
    std::string stdout_text;
};

RunResult run_command_capture(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");
    std::string output;
    if (pipe != nullptr) {
        char buffer[256];
        std::size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            output.append(buffer, n);
        }
    }
    int status = pipe != nullptr ? pclose(pipe) : -1;
    return RunResult{pipe != nullptr && WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

void expect_dwarf_variable_has_location(const std::filesystem::path& binary_path, const std::string& variable_name,
                                        const std::string& case_name) {
    RunResult dwarfdump_result =
        run_command_capture("llvm-dwarfdump-22 --debug-info \"" + binary_path.string() + "\" 2>&1");
    expect(dwarfdump_result.exit_code == 0,
           case_name + ": llvm-dwarfdump should succeed, got '" + dwarfdump_result.stdout_text + "'");
    if (dwarfdump_result.exit_code != 0) return;
    std::string needle = "DW_AT_name\t(\"" + variable_name + "\")";
    std::size_t name_pos = dwarfdump_result.stdout_text.find(needle);
    expect(name_pos != std::string::npos,
           case_name + ": expected DWARF entry for variable '" + variable_name + "', got '" +
               dwarfdump_result.stdout_text + "'");
    if (name_pos == std::string::npos) return;
    std::size_t entry_begin = dwarfdump_result.stdout_text.rfind("DW_TAG_variable", name_pos);
    expect(entry_begin != std::string::npos,
           case_name + ": expected DW_TAG_variable for '" + variable_name + "', got '" +
               dwarfdump_result.stdout_text + "'");
    if (entry_begin == std::string::npos) return;
    std::size_t entry_end = dwarfdump_result.stdout_text.find("DW_TAG_", name_pos + needle.size());
    std::string entry =
        dwarfdump_result.stdout_text.substr(entry_begin, entry_end == std::string::npos ? std::string::npos
                                                                                       : entry_end - entry_begin);
    expect(entry.find("DW_AT_location") != std::string::npos,
           case_name + ": expected DW_AT_location for variable '" + variable_name + "', got '" + entry + "'");
}

void expect_dwarf_named_entry_contains(const std::filesystem::path& binary_path, const std::string& tag_name,
                                       const std::string& entry_name, const std::string& expected_text,
                                       const std::string& case_name) {
    RunResult dwarfdump_result =
        run_command_capture("llvm-dwarfdump-22 --debug-info \"" + binary_path.string() + "\" 2>&1");
    expect(dwarfdump_result.exit_code == 0,
           case_name + ": llvm-dwarfdump should succeed, got '" + dwarfdump_result.stdout_text + "'");
    if (dwarfdump_result.exit_code != 0) return;
    std::string needle = "DW_AT_name\t(\"" + entry_name + "\")";
    std::size_t name_pos = dwarfdump_result.stdout_text.find(needle);
    expect(name_pos != std::string::npos,
           case_name + ": expected DWARF entry named '" + entry_name + "', got '" + dwarfdump_result.stdout_text + "'");
    if (name_pos == std::string::npos) return;
    std::size_t entry_begin = dwarfdump_result.stdout_text.rfind(tag_name, name_pos);
    expect(entry_begin != std::string::npos,
           case_name + ": expected " + tag_name + " for '" + entry_name + "', got '" + dwarfdump_result.stdout_text + "'");
    if (entry_begin == std::string::npos) return;
    std::size_t entry_end = dwarfdump_result.stdout_text.find("DW_TAG_", name_pos + needle.size());
    std::string entry =
        dwarfdump_result.stdout_text.substr(entry_begin, entry_end == std::string::npos ? std::string::npos
                                                                                       : entry_end - entry_begin);
    expect(entry.find(expected_text) != std::string::npos,
           case_name + ": expected DWARF entry for '" + entry_name + "' to contain '" + expected_text + "', got '" + entry + "'");
}

void write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path);
    file << content;
}

std::string shell_quote(const std::string& text) {
    std::string quoted = "'";
    for (char ch : text) {
        if (ch == '\'') {
            quoted += "'\"'\"'";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

// Compiles `source` to a temporary executable, runs it, and captures both
// its stdout and exit code (0-255, matching POSIX wait status semantics).
std::expected<RunResult, scpp::DriverError> try_compile_and_run(std::string_view source, const std::string& case_name) {
    std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
    auto compile_result_1 = scpp::compile_to_executable(source, exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
    if (!compile_result_1.has_value()) return std::unexpected(std::move(compile_result_1).error());

    FILE* pipe = popen(exe_path.string().c_str(), "r");
    std::string output;
    if (pipe != nullptr) {
        char buffer[256];
        std::size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            output.append(buffer, n);
        }
    }
    int status = pipe != nullptr ? pclose(pipe) : -1;

    std::filesystem::remove(exe_path);
    return RunResult{WEXITSTATUS(status), output};
}

RunResult compile_and_run(std::string_view source, const std::string& case_name) {
    auto result = try_compile_and_run(source, case_name);
    if (!result.has_value()) throw std::move(result).error();
    return std::move(result).value();
}

std::expected<RunResult, scpp::DriverError> try_compile_and_run_with_input(std::string_view source,
                                                                            const std::string& case_name,
                                                                            std::string_view input) {
    std::filesystem::path exe_path = std::filesystem::current_path() / ("scpp_driver_test_" + case_name);
    auto compile_result_2 = scpp::compile_to_executable(source, exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
    if (!compile_result_2.has_value()) return std::unexpected(std::move(compile_result_2).error());
    RunResult result = run_command_capture("printf %s " + shell_quote(std::string(input)) + " | " +
                                           shell_quote(exe_path.string()) + " 2>&1");
    std::filesystem::remove(exe_path);
    return result;
}

RunResult compile_and_run_with_input(std::string_view source, const std::string& case_name, std::string_view input) {
    auto result = try_compile_and_run_with_input(source, case_name, input);
    if (!result.has_value()) throw std::move(result).error();
    return std::move(result).value();
}

// A `<name>.expected` file's first line is the expected exit code; anything
// after the first newline is the expected stdout, compared exactly.
struct ExpectedResult {
    int exit_code;
    std::string stdout_text;
};

ExpectedResult parse_expected(const std::string& content) {
    std::size_t newline = content.find('\n');
    std::string exit_code_line = newline == std::string::npos ? content : content.substr(0, newline);
    std::string stdout_text = newline == std::string::npos ? "" : content.substr(newline + 1);
    return ExpectedResult{std::stoi(exit_code_line), stdout_text};
}

// Runs every `<name>.scpp` case file under `dir` against its paired
// `<name>.expected` file (see parse_expected). Adding a new test case is just
// dropping in 2 new files -- no changes to this file or a rebuild of the test
// harness are needed, just re-running the already-built binary.
void run_runtime_test_case_files(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> source_files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".scpp") {
            source_files.push_back(entry.path());
        }
    }
    std::sort(source_files.begin(), source_files.end());

    expect(!source_files.empty(), "expected at least one *.scpp test case in " + dir.string());

    for (const std::filesystem::path& source_path : source_files) {
        std::string case_name = source_path.stem().string();
        std::filesystem::path expected_path = dir / (case_name + ".expected");
        if (!std::filesystem::exists(expected_path)) {
            expect(false, case_name + ": missing " + expected_path.filename().string());
            continue;
        }

        ExpectedResult expected = parse_expected(read_file(expected_path));
        cases_run++;

        auto result = try_compile_and_run(read_file(source_path), case_name);
        if (!result.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(result.error().what()));
            continue;
        }
        expect(result->exit_code == expected.exit_code, case_name + ": expected exit code " +
                                                             std::to_string(expected.exit_code) + ", got " +
                                                             std::to_string(result->exit_code));
        expect(result->stdout_text == expected.stdout_text, case_name + ": expected stdout '" +
                                                                 expected.stdout_text + "', got '" +
                                                                 result->stdout_text + "'");
    }
}

void run_test_case_files() {
    run_runtime_test_case_files(SCPP_TEST_SOURCE_DIR);
}

void run_driver_single_test_case_files() {
    run_runtime_test_case_files(std::filesystem::path(SCPP_DRIVER_TEST_SOURCE_DIR) / "single");
}

// Regression coverage for the clang/gcc-style diagnostic location plumbing
// (SourceLocation, ast.cppm; ParseError::loc/DataflowError::loc/
// CodegenError::loc): a handful of known-bad snippets, each checked for
// the *specific* error type and a plausible (non-zero, matching) line/
// column -- not the rendered "file:line:col: error: ..." text itself,
// which is cli.cppm's own presentation concern (an unexported, CLI-only
// helper untestable from here without shelling out to the built `scpp`
// binary; the blackbox_test/ suite -- a separate, independently
// maintained black-box harness -- is where exercising the CLI's actual
// stderr output belongs, not this binary).
void run_error_location_tests() {
    struct Case {
        std::string name;
        std::string source;
        int expected_line;
    };
    std::vector<Case> parse_cases = {
        {"missing_semicolon", "int main() {\n    int x = 5\n    return 0;\n}\n", 3},
    };
    for (const Case& c : parse_cases) {
        cases_run++;
        if (auto result = scpp::parse(c.source); !result.has_value()) {
            const scpp::ParseError& e = result.error();
            expect(e.loc.is_known(), c.name + ": ParseError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
        } else {
            expect(false, c.name + ": expected a ParseError, none was thrown");
        }
    }

    std::vector<Case> dataflow_cases = {
        {"use_after_move",
         "import std;\nint f() {\n    std::unique_ptr<int> p = std::make_unique<int>(5);\n    std::unique_ptr<int> q = "
         "std::move(p);\n    return *p;\n}\nint main() { return f(); }\n",
         5},
    };
    for (const Case& c : dataflow_cases) {
        cases_run++;
        scpp::Program program = parse_with_std_imports(c.source);
        auto monomorphize_result = scpp::monomorphize_generics(program);
        if (!monomorphize_result.has_value()) {
            const scpp::DataflowError& e = monomorphize_result.error();
            expect(e.loc.is_known(), c.name + ": DataflowError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
            continue;
        }
        auto check_moves_result = scpp::check_moves(program);
        if (!check_moves_result.has_value()) {
            const scpp::DataflowError& e = check_moves_result.error();
            expect(e.loc.is_known(), c.name + ": DataflowError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
            continue;
        }
        expect(false, c.name + ": expected a DataflowError, none was thrown");
    }

    std::vector<Case> codegen_cases = {
        {"bool_int_mismatch", "int main() {\n    bool b = 5;\n    return 0;\n}\n", 2},
    };
    for (const Case& c : codegen_cases) {
        cases_run++;
        auto program_result = scpp::parse(c.source);
        if (!program_result.has_value()) {
            expect(false, c.name + ": expected a CodegenError, none was thrown");
            continue;
        }
        scpp::Program program = std::move(program_result.value());
        scpp::Codegen codegen("test_module");
        auto generate_result = codegen.generate(program);
        if (!generate_result.has_value()) {
            const scpp::CodegenError& e = generate_result.error();
            expect(e.loc.is_known(), c.name + ": CodegenError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
        } else {
            expect(false, c.name + ": expected a CodegenError, none was thrown");
        }
    }
}

// ch11 (Modules & Libraries): end-to-end coverage for the multi-file
// module system -- real separate compilation (each module gets its own
// object file), cross-module signature recovery seeding movecheck with
// zero new checker logic (§11.8), and ch11 §11.9's real mangling scheme,
// all exercised through the actual public API (scpp::parse's
// ModuleResolver + scpp::compile_to_executable's import_paths) rather
// than by poking any single layer in isolation. Writes each case's
// imported module source to a real temp file, since ModuleCache (driver.
// cppm) resolves `--import name=path` against an actual file path, not
// an in-memory string.
void run_module_system_tests() {
    auto write_temp_file = [](const std::string& case_name, const std::string& suffix, const std::string& content) {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_" + suffix + ".scpp");
        std::ofstream file(path);
        file << content;
        file.close();
        return path;
    };

    // A basic import: an exported function and an exported class, called
    // from a plain (non-module) consumer file.
    {
        std::string case_name = "module_basic_import";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "namespace mathlib {\n"
            "    export int square(int x) { return x * x; }\n"
            "    int helper(int x) { return x + 1; }\n"
            "    export int square_plus_one(int x) { return mathlib::square(x) + mathlib::helper(0); }\n"
            "}\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() {\n"
            "    print_int(mathlib::square(6));\n"
            "    print_int(mathlib::square_plus_one(6));\n"
            "    return 0;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_3 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()}});
        if (!compile_result_3.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_3.error().what()));
        } else {
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                std::size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "36\n37\n", case_name + ": expected stdout '36\\n37\\n', got '" + output + "'");
        }
        std::filesystem::remove(lib_path);
    }

    // A module-private (non-exported) function is invisible to an
    // importer -- calling it should fail exactly like calling any other
    // undeclared name (ch11 §11.3: only `export`-marked declarations
    // cross the module boundary at all). Movecheck itself has never
    // rejected a call to a genuinely unknown name (see
    // check_call_arguments's own comment -- that's codegen's "call to
    // unknown function" check), so this needs to run codegen too, not
    // movecheck alone.
    {
        std::string case_name = "module_private_function_not_visible";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "namespace mathlib {\n"
            "    int helper(int x) { return x + 1; }\n"
            "}\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() {\n"
            "    print_int(mathlib::helper(6));\n"
            "    return 0;\n"
            "}\n";
        bool threw = [&] {
            auto lib_program_result = scpp::parse(read_file(lib_path));
            if (!lib_program_result.has_value()) return true;
            scpp::Program lib_program = std::move(lib_program_result.value());
            auto program_result = scpp::parse(
                main_source, [&lib_program](const std::string&) -> const scpp::Program* { return &lib_program; });
            if (!program_result.has_value()) return true;
            scpp::Program program = std::move(program_result.value());
            auto check_moves_result = scpp::check_moves(program);
            if (!check_moves_result.has_value()) return true;
            scpp::Codegen codegen("test_module");
            auto generate_result = codegen.generate(program);
            return !generate_result.has_value();
        }();
        expect(threw, case_name + ": expected calling a module-private function to fail");
        std::filesystem::remove(lib_path);
    }

    // Ordinary forward declarations should work inside a module interface
    // unit too, including an exported declaration whose later definition
    // omits a second `export`.
    {
        std::string case_name = "module_forward_declarations_enable_mutual_recursion";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "namespace mathlib {\n"
            "    export int is_even(int x);\n"
            "    int is_odd(int x);\n"
            "    int is_even(int x) {\n"
            "        if (x == 0) return 1;\n"
            "        return is_odd(x - 1);\n"
            "    }\n"
            "    int is_odd(int x) {\n"
            "        if (x == 0) return 0;\n"
            "        return is_even(x - 1);\n"
            "    }\n"
            "}\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() {\n"
            "    return mathlib::is_even(6) - 1;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_4 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()}});
        if (!compile_result_4.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_4.error().what()));
        } else {
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(run.exit_code));
        }
        std::filesystem::remove(lib_path);
    }

    {
        std::string case_name = "module_record_forward_declarations_reconcile_to_full_definitions";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module records;\n"
            "namespace records {\n"
            "    export struct Node;\n"
            "    struct Node { int value; Node* next; };\n"
            "    export Node make_node(int value) {\n"
            "        Node node{};\n"
            "        node.value = value;\n"
            "        return node;\n"
            "    }\n"
            "}\n");
        std::string main_source =
            "import records;\n"
            "int main() {\n"
            "    records::Node node = records::make_node(7);\n"
            "    return node.value - 7;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_5 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"records", lib_path.string()}});
        if (!compile_result_5.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_5.error().what()));
        } else {
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(run.exit_code));
        }
        std::filesystem::remove(lib_path);
    }

    // Importing a module whose own module declaration doesn't match the
    // requested name is rejected (a mismatched --import name=path).
    {
        std::string case_name = "module_name_mismatch_is_rejected";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib", "export module actuallib;\n");
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_6 = scpp::compile_to_executable("import mathlib;\nint main() { return 0; }\n", exe_path.string(), {},
                                     {{"mathlib", lib_path.string()}});
        bool threw = !compile_result_6.has_value();
        if (!threw) std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected a DriverError for a module name mismatch");
        std::filesystem::remove(lib_path);
    }

    // Importing a module with no corresponding --import mapping at all
    // is rejected with a clear error, not a crash.
    {
        std::string case_name = "missing_import_mapping_is_rejected";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_7 = scpp::compile_to_executable("import nonexistent;\nint main() { return 0; }\n", exe_path.string());
        bool threw = !compile_result_7.has_value();
        if (!threw) std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected a DriverError for a missing --import mapping");
    }

    {
        std::string case_name = "import_search_dir_resolves_module";
        cases_run++;
        std::filesystem::path module_dir = std::filesystem::current_path() / "driver_import_search_dir_case";
        std::filesystem::create_directories(module_dir);
        std::filesystem::path module_path = module_dir / "mathlib.scpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "driver_import_search_dir_case_exe";
        write_text_file(module_path,
                        "export module mathlib;\n"
                        "namespace mathlib { export int value() { return 17; } }\n");
        auto compile_result_8 = scpp::compile_to_executable("import mathlib;\nint main() { return mathlib::value(); }\n", exe_path.string(), {},
                                    {}, /*static_link=*/false, {module_dir.string()});
        if (!compile_result_8.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_8.error().what()));
        } else {
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            expect(run.exit_code == 17,
                   case_name + ": expected exit code 17, got " + std::to_string(run.exit_code));
        }
        std::filesystem::remove(module_path);
        std::filesystem::remove(exe_path);
        std::filesystem::remove(module_dir);
    }

    {
        std::string case_name = "import_search_dir_first_match_wins";
        cases_run++;
        std::filesystem::path first_dir = std::filesystem::current_path() / "driver_import_search_dir_first";
        std::filesystem::path second_dir = std::filesystem::current_path() / "driver_import_search_dir_second";
        std::filesystem::create_directories(first_dir);
        std::filesystem::create_directories(second_dir);
        write_text_file(first_dir / "mathlib.scpp",
                        "export module mathlib;\n"
                        "namespace mathlib { export int value() { return 11; } }\n");
        write_text_file(second_dir / "mathlib.scpp",
                        "export module mathlib;\n"
                        "namespace mathlib { export int value() { return 22; } }\n");
        std::filesystem::path exe_path = std::filesystem::current_path() / "driver_import_search_dir_first_match_exe";
        auto compile_result_9 = scpp::compile_to_executable("import mathlib;\nint main() { return mathlib::value(); }\n", exe_path.string(), {},
                                    {}, /*static_link=*/false, {first_dir.string(), second_dir.string()});
        if (!compile_result_9.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_9.error().what()));
        } else {
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            expect(run.exit_code == 11,
                   case_name + ": expected first -I directory to win, got exit code " +
                       std::to_string(run.exit_code));
        }
        std::filesystem::remove(first_dir / "mathlib.scpp");
        std::filesystem::remove(second_dir / "mathlib.scpp");
        std::filesystem::remove(exe_path);
        std::filesystem::remove(first_dir);
        std::filesystem::remove(second_dir);
    }

    // A direct circular import (A imports B, B imports A) is rejected
    // rather than infinite-recursing.
    {
        std::string case_name = "circular_import_is_rejected";
        cases_run++;
        std::filesystem::path a_path = write_temp_file(case_name, "a", "export module a;\nimport b;\n");
        std::filesystem::path b_path = write_temp_file(case_name, "b", "export module b;\nimport a;\n");
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_10 = scpp::compile_to_executable("import a;\nint main() { return 0; }\n", exe_path.string(), {},
                                     {{"a", a_path.string()}, {"b", b_path.string()}});
        bool threw = !compile_result_10.has_value();
        if (!threw) std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected a DriverError for a circular import");
        std::filesystem::remove(a_path);
        std::filesystem::remove(b_path);
    }

    // ch11 §11.4: module partitions, end-to-end through the real CLI-level
    // API. A primary interface unit (mathlib) aggregates an interface
    // partition (mathlib:trig) via `export import :trig;`, and the whole
    // module compiles+links+runs together with a plain consumer.
    {
        std::string case_name = "partition_export_import_end_to_end";
        cases_run++;
        std::filesystem::path trig_path = write_temp_file(case_name, "trig",
            "export module mathlib:trig;\n"
            "namespace mathlib {\n"
            "    export int sin_deg_approx(int degrees) { return degrees / 2; }\n"
            "    int private_helper(int x) { return x + 1000; }\n"
            "}\n");
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "export import :trig;\n"
            "namespace mathlib {\n"
            "    export int square(int x) { return x * x; }\n"
            "}\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() {\n"
            "    print_int(mathlib::square(6));\n"
            "    print_int(mathlib::sin_deg_approx(90));\n"
            "    return 0;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_11 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
        if (!compile_result_11.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_11.error().what()));
        } else {
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                std::size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "36\n45\n", case_name + ": expected stdout '36\\n45\\n', got '" + output + "'");
        }
        std::filesystem::remove(trig_path);
        std::filesystem::remove(lib_path);
    }

    // A partition's own non-exported declaration stays invisible to an
    // external importer of the whole module, even though the primary
    // unit does `export import :trig;` -- only what the partition itself
    // marked `export` gets re-exported.
    {
        std::string case_name = "partition_private_declaration_not_reexported";
        cases_run++;
        std::filesystem::path trig_path = write_temp_file(case_name, "trig",
            "export module mathlib:trig;\n"
            "namespace mathlib { int private_helper(int x) { return x; } }\n");
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "export import :trig;\n"
            "namespace mathlib { export int square(int x) { return x * x; } }\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() { print_int(mathlib::private_helper(1)); return 0; }\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_12 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
        bool threw = !compile_result_12.has_value();
        if (!threw) std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected private_helper to stay invisible outside the module");
        std::filesystem::remove(trig_path);
        std::filesystem::remove(lib_path);
    }

    // A plain `import :part;` (no `export`) uses a partition internally
    // but never re-exports it -- even an exported-within-the-partition
    // declaration stays invisible to an external importer of the module.
    {
        std::string case_name = "plain_partition_import_not_visible_externally";
        cases_run++;
        std::filesystem::path trig_path = write_temp_file(case_name, "trig",
            "export module mathlib:trig;\n"
            "namespace mathlib { export int sin_deg_approx(int degrees) { return degrees / 2; } }\n");
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "import :trig;\n"
            "namespace mathlib { export int square(int x) { return x * x; } }\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() { print_int(mathlib::sin_deg_approx(90)); return 0; }\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_13 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
        bool threw = !compile_result_13.has_value();
        if (!threw) std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected sin_deg_approx to stay invisible (plain import :part; never "
                                   "re-exports)");
        std::filesystem::remove(trig_path);
        std::filesystem::remove(lib_path);
    }

    // A non-exported declaration inside one partition is still visible to
    // another partition of the same module that imports it privately; only
    // *external* importers are blocked from naming that helper.
    {
        std::string case_name = "partition_private_helper_visible_inside_same_module";
        cases_run++;
        std::filesystem::path helper_path = write_temp_file(case_name, "helper",
            "export module mathlib:helper;\n"
            "namespace mathlib { int hidden_twice(int x) { return x * 2; } }\n");
        std::filesystem::path api_path = write_temp_file(case_name, "api",
            "export module mathlib:api;\n"
            "import :helper;\n"
            "namespace mathlib { export int call_hidden(int x) { return hidden_twice(x); } }\n");
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module mathlib;\n"
            "export import :api;\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() { return mathlib::call_hidden(21) - 42; }\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_14 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", lib_path.string()},
                                      {"mathlib:api", api_path.string()},
                                      {"mathlib:helper", helper_path.string()}});
        if (!compile_result_14.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_14.error().what()));
        } else {
            RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run_result.exit_code == 0,
                   case_name + ": expected same-module private helper call to succeed, got " +
                       std::to_string(run_result.exit_code));
        }
        std::filesystem::remove(helper_path);
        std::filesystem::remove(api_path);
        std::filesystem::remove(lib_path);
    }

    // ch11 §11.8: `export import a;` inside module `b` re-exports `a`'s
    // exports *transitively* -- a third file that only `import b;` (never
    // importing `a` directly) can still call `a::value()` by relying on
    // that transitive re-export. This is also a mangling-correctness
    // regression test: the symbol codegen declares/defines for
    // `a::value()` must be mangled using "a" (its *original* defining
    // module) even when merged a second time via "b" -- a real bug found
    // by black-box testing (colleague-reported): the merged clone's
    // owning_module was being unconditionally overwritten with the
    // *re-exporting* module's name ("b") instead of preserving the
    // original ("a"), producing a mangled symbol nothing ever defined
    // and failing to link.
    {
        std::string case_name = "export_import_reexports_transitively";
        cases_run++;
        std::filesystem::path a_path = write_temp_file(case_name, "a",
            "export module a;\nnamespace a { export int value() { return 42; } }\n");
        std::filesystem::path b_path = write_temp_file(case_name, "b",
            "export module b;\n"
            "export import a;\n"
            "namespace b { export int helper() { return a::value() + 1; } }\n");
        std::string main_source =
            "import b;\n"
            "int main() {\n"
            "    print_int(a::value());\n"
            "    print_int(b::helper());\n"
            "    return 0;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_15 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"a", a_path.string()}, {"b", b_path.string()}});
        if (!compile_result_15.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_15.error().what()));
        } else {
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                std::size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "42\n43\n", case_name + ": expected stdout '42\\n43\\n', got '" + output + "'");
        }
        std::filesystem::remove(a_path);
        std::filesystem::remove(b_path);
    }

    {
        std::string case_name = "export_namespace_block_exports_members";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module exportnsblock;\n"
            "export namespace inner {\n"
            "    int increment(int x) { return x + 1; }\n"
            "    int helper(int x) { return increment(x); }\n"
            "}\n");
        std::string main_source =
            "import exportnsblock;\n"
            "int main() {\n"
            "    return inner::helper(41) - 42;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_16 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"exportnsblock", lib_path.string()}});
        if (!compile_result_16.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_16.error().what()));
        } else {
            RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run_result.exit_code == 0,
                   case_name + ": expected export-namespace members to import and run, got " +
                       std::to_string(run_result.exit_code));
        }
        std::filesystem::remove(lib_path);
    }

    {
        std::string case_name = "direct_and_transitive_partition_reexports_do_not_duplicate_decls";
        cases_run++;
        std::filesystem::path base_path = write_temp_file(case_name, "base",
            "export module mathlib:base;\n"
            "namespace mathlib { export int value() { return 42; } }\n");
        std::filesystem::path random_path = write_temp_file(case_name, "random",
            "export module mathlib:random;\n"
            "export import :base;\n"
            "namespace mathlib { export int helper() { return value() + 1; } }\n");
        std::filesystem::path mathlib_path = write_temp_file(case_name, "mathlib",
            "export module mathlib;\n"
            "export import :base;\n"
            "export import :random;\n");
        std::string main_source =
            "import mathlib;\n"
            "int main() {\n"
            "    print_int(mathlib::value());\n"
            "    print_int(mathlib::helper());\n"
            "    return 0;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_17 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"mathlib", mathlib_path.string()},
                                      {"mathlib:base", base_path.string()},
                                      {"mathlib:random", random_path.string()}});
        if (!compile_result_17.has_value()) {
            expect(false, case_name + ": threw an exception: " + std::string(compile_result_17.error().what()));
        } else {
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                std::size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "42\n43\n", case_name + ": expected stdout '42\\n43\\n', got '" + output + "'");
        }
        std::filesystem::remove(base_path);
        std::filesystem::remove(random_path);
        std::filesystem::remove(mathlib_path);
    }

    // ch11 §11.8: a plain (non-reexporting) `import a;` inside module `b`
    // is private -- `a`'s exports must NOT become visible to a third file
    // that only `import b;`, even though `b`'s own code (e.g. `helper()`)
    // can still call `a::value()` internally without issue.
    {
        std::string case_name = "plain_import_does_not_reexport_transitively";
        cases_run++;
        std::filesystem::path a_path = write_temp_file(case_name, "a",
            "export module a;\nnamespace a { export int value() { return 42; } }\n");
        std::filesystem::path b_path = write_temp_file(case_name, "b",
            "export module b;\n"
            "import a;\n"
            "namespace b { export int helper() { return a::value() + 1; } }\n");

        // The indirect call (through b::helper()) must still work fine.
        {
            std::string main_source = "import b;\nint main() { print_int(b::helper()); return 0; }\n";
            std::filesystem::path exe_path = std::filesystem::temp_directory_path() /
                                              ("scpp_driver_test_" + case_name + "_indirect_exe");
            auto compile_result_18 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"a", a_path.string()}, {"b", b_path.string()}});
            if (!compile_result_18.has_value()) {
                expect(false, case_name + " (indirect): threw an exception: " + std::string(compile_result_18.error().what()));
            } else {
                FILE* pipe = popen(exe_path.string().c_str(), "r");
                std::string output;
                if (pipe != nullptr) {
                    char buffer[256];
                    std::size_t n;
                    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
                }
                int status = pipe != nullptr ? pclose(pipe) : -1;
                std::filesystem::remove(exe_path);
                expect(WEXITSTATUS(status) == 0, case_name + " (indirect): expected exit code 0");
                expect(output == "43\n", case_name + " (indirect): expected stdout '43\\n', got '" + output + "'");
            }
        }

        // The direct call (relying on transitive visibility through a
        // private import) must be rejected.
        {
            cases_run++;
            std::string main_source = "import b;\nint main() { print_int(a::value()); return 0; }\n";
            std::filesystem::path exe_path = std::filesystem::temp_directory_path() /
                                              ("scpp_driver_test_" + case_name + "_direct_exe");
            auto compile_result_19 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"a", a_path.string()}, {"b", b_path.string()}});
            bool threw = !compile_result_19.has_value();
            if (!threw) std::filesystem::remove(exe_path);
            expect(threw, case_name + " (direct): expected a::value() to stay invisible (private import is "
                                       "non-transitive)");
        }
        std::filesystem::remove(a_path);
        std::filesystem::remove(b_path);
    }

    // Regression test for a real compiler bug in movecheck's
    // validate_constructor_member_initialization (compiler/movecheck/
    // signatures.cppm): a non-template class's constructor recovered
    // from an imported module keeps its member_initializers list but has
    // its own body cleared during the cross-module merge (see
    // clone_function_declaration's `keep_body` parameter -- only a
    // generic template keeps a body when merged, so the importer can
    // monomorphize it locally; an ordinary class's constructor is
    // declaration-only and defined by the imported module's own object
    // file). check_moves used to re-validate this now-bodyless, already
    // -defined-elsewhere constructor as if it were a fresh local
    // definition, incorrectly flagging its own field as uninitialized
    // even though the field genuinely is initialized -- just by a
    // member-initializer-list this recovered declaration no longer
    // carries a body to display. Fixed by an early-return guard for a
    // bodyless constructor (necessarily a merged, already-validated-once
    // redeclaration, never a fresh local one needing validation).
    {
        std::string case_name = "imported_class_constructor_with_member_initializer_list_builds";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib",
            "export module dm_holder;\n"
            "namespace dm_holder {\n"
            "    export struct Holder {\n"
            "        int value_;\n"
            "        Holder(int value) : value_{value} {}\n"
            "    };\n"
            "}\n");
        std::string main_source =
            "import dm_holder;\n"
            "int main() {\n"
            "    dm_holder::Holder h{5};\n"
            "    return h.value_ - 5;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_20 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"dm_holder", lib_path.string()}});
        if (!compile_result_20.has_value()) {
            expect(false, case_name + ": threw an exception (movecheck incorrectly rejecting a merged, "
                                       "already-validated constructor as having an uninitialized field): " +
                              std::string(compile_result_20.error().what()));
        } else {
            RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run_result.exit_code == 0,
                   case_name + ": expected the imported class's member-initializer-list constructor to build and "
                               "run correctly, got exit " + std::to_string(run_result.exit_code) + " output '" +
                               run_result.stdout_text + "'");
        }
        std::filesystem::remove(lib_path);
    }

    // Regression test for a real compiler bug in codegen/orchestration.
    // cppm: a defaulted special member (e.g. `virtual ~Widget() =
    // default;`) recovered from an imported-and-*re-exported* module
    // keeps its is_defaulted flag through clone_function_declaration
    // (only the clone's body is cleared for non-template functions --
    // see merge_imported_module's own comment), and is also externally
    // linked here (declare_function's has_definition/is_exported check)
    // since a re-export keeps the clone's is_exported flag too (see
    // clone_function_declaration's `is_reexport && fn.is_exported`).
    // Before the fix, codegen's top-level definition loop synthesized a
    // *fresh* external definition for this defaulted destructor in
    // every module that merged it, regardless of whether it was
    // recovered from an already-independently-compiled owning module --
    // producing two externally linked, identically mangled definitions
    // (one from "dm_a"'s own compilation, one redundantly re-synthesized
    // while compiling "dm_b") and failing to link with a "multiple
    // definition" error. This is the same real bug that showed up
    // between libstd.scppa and libscpp.scppa for std::runtime_error's
    // destructor (libs/scpp/rand/scpp_rand.scpp re-exports std),
    // reproduced here with two small first-class modules instead.
    {
        std::string case_name = "reexported_defaulted_destructor_does_not_duplicate_definition";
        cases_run++;
        std::filesystem::path a_path = write_temp_file(case_name, "a",
            "export module dm_a;\n"
            "namespace dm_a {\n"
            "    export class Widget {\n"
            "    public:\n"
            "        virtual ~Widget() = default;\n"
            "        int tag() { return 7; }\n"
            "    };\n"
            "}\n");
        std::filesystem::path b_path = write_temp_file(case_name, "b",
            "export module dm_b;\n"
            "export import dm_a;\n");
        std::string main_source =
            "import dm_b;\n"
            "int main() {\n"
            "    dm_a::Widget w{};\n"
            "    return w.tag() - 7;\n"
            "}\n";
        std::filesystem::path exe_path =
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
        auto compile_result_21 = scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                     {{"dm_a", a_path.string()}, {"dm_b", b_path.string()}});
        if (!compile_result_21.has_value()) {
            expect(false, case_name + ": threw an exception (this used to be a link-time \"multiple definition\" "
                                       "error before the fix): " + std::string(compile_result_21.error().what()));
        } else {
            RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
            std::filesystem::remove(exe_path);
            expect(run_result.exit_code == 0,
                   case_name + ": expected build+link to succeed with the re-exported defaulted destructor not "
                               "duplicating its definition, got exit " + std::to_string(run_result.exit_code) +
                               " output '" + run_result.stdout_text + "'");
        }
        std::filesystem::remove(a_path);
        std::filesystem::remove(b_path);
    }
}

// ch05 §5.11: generic functions/concepts -- monomorphization end-to-end
// and the "checked once, abstractly, zero new movecheck logic" claim.
// Lives here (not movetest_source/codegentest_source) because it
// genuinely needs the *full* pipeline (parse -> monomorphize_generics ->
// check_moves -> codegen) in a single test: movetest_source's own
// throws_move_error helper never runs codegen at all (see
// movecheck_test.cpp), and codegentest_source's generate_ir never runs
// monomorphize_generics (deliberately testing codegen in isolation --
// see codegen_test.cpp), so neither alone can reach this specific
// rejection (an "unknown method" inside a *monomorphized clone's* body,
// which only exists once a real call site triggers instantiation).
void run_concept_tests() {
    // A generic function's own body is checked once, abstractly, against
    // its constrained parameter's witness class -- calling an operation
    // the concept never promised (here `.perimeter()`, which `Shape`
    // never requires) is rejected via the exact same "unknown method"
    // mechanism an ordinary class-typed call would hit, with zero new
    // movecheck logic. Like module_private_function_not_visible above,
    // this surfaces at codegen's own "unknown function" check (an
    // unresolved callee name has never been movecheck's own job -- see
    // check_call_arguments's comment), reached only once a real call
    // site (print_area(c) below) triggers monomorphize_generics to
    // produce a concrete clone whose body still calls the ungranted
    // operation, now against the concrete type's own naming scheme.
    {
        std::string case_name = "concept_generic_body_calling_ungranted_operation_is_rejected";
        cases_run++;
        std::string source =
            "class Circle {\n"
            "public:\n"
            "    virtual ~Circle() = default;\n"
            "    Circle() { return; }\n"
            "    int area() const { return 314; }\n"
            "};\n"
            "template<typename T>\n"
            "concept Shape = requires(const T& t) {\n"
            "    { t.area() } -> std::same_as<int>;\n"
            "};\n"
            "int print_area(const Shape auto& s) {\n"
            "    return s.perimeter();\n"
            "}\n"
            "int main() {\n"
            "    Circle c{};\n"
            "    return print_area(c);\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(threw, case_name + ": expected calling an operation not promised by the concept to fail");
    }
}

// ch05 §5.14: generic types (classes/structs) -- same "needs the full
// pipeline in one test" reasoning as run_concept_tests just above: a
// generic class method's own `requires Concept<T>` clause is only
// checked once a real instantiation (`Vec<SomeType>`) exists at all
// (movecheck's Monomorphizer, resolve_generic_types), and calling a
// method whose constraint the concrete argument doesn't satisfy is
// rejected via the same "unknown function" mechanism as an ordinary
// unresolved callee -- codegen's own job, unreachable through
// movetest_source's movecheck-only throws_move_error helper.
void run_generic_type_tests() {
    {
        std::string case_name = "full_class_template_specialization_overrides_primary_definition";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
template<typename T>
class Selector {
public:
    int value() const { return 1; }
    virtual ~Selector() = default;
};
template<>
class Selector<int> {
public:
    int value() const { return 2; }
    virtual ~Selector() = default;
};
int main() {
    Selector<char> fallback{};
    Selector<int> chosen{};
    if (fallback.value() != 1) return 1;
    return chosen.value() == 2 ? 0 : 2;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string source =
            "template<typename T>\n"
            "concept Describable = requires(const T& t) {\n"
            "    { t.magnitude() } -> std::same_as<int>;\n"
            "};\n"
            "class NoMagnitude {\n"
            "public:\n"
            "    virtual ~NoMagnitude() = default;\n"
            "    NoMagnitude(int v) : value{v} { return; }\n"
            "private:\n"
            "    int value{};\n"
            "};\n"
            "template<typename T>\n"
            "class Vec {\n"
            "    T item;\n"
            "public:\n"
            "    virtual ~Vec() = default;\n"
            "    Vec(const T& x) : item{x} { return; }\n"
            "    int describe() const requires Describable<T> {\n"
            "        return this.item.magnitude();\n"
            "    }\n"
            "};\n"
            "int main() {\n"
            "    NoMagnitude n{1};\n"
            "    Vec<NoMagnitude> vn{n};\n"
            "    return vn.describe();\n"
            "}\n";
        std::string case_name = "generic_class_constrained_method_unsatisfying_type_is_rejected";
        cases_run++;
        bool threw = full_pipeline_fails(source);
        expect(threw, case_name + ": expected calling a method whose own requires-clause the concrete type "
                                  "argument doesn't satisfy to fail");
    }

    {
        std::string source =
            "template<typename... Ts>\n"
            "class Box;\n"
            "\n"
            "template<>\n"
            "class Box<> {\n"
            "public:\n"
            "    virtual ~Box() { return; }\n"
            "    Box() { return; }\n"
            "    Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    virtual ~Box() override { return; }\n"
            "    Box() { return; }\n"
            "    Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> b{\"hi\"};\n"
            "    return 0;\n"
            "}\n";
        std::string case_name = "variadic_generic_instantiation_clones_constructor";
        cases_run++;
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected concrete variadic instantiations to inherit cloned constructors");
    }

    {
        std::string source =
            "template<typename... Ts>\n"
            "class Box;\n"
            "\n"
            "template<>\n"
            "class Box<> {\n"
            "public:\n"
            "    virtual ~Box() { return; }\n"
            "    int size() const { return 10; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    virtual ~Box() override { return; }\n"
            "    int size() const { return 50; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> b{};\n"
            "    return b.size() - 50;\n"
            "}\n";
        std::string case_name = "variadic_generic_instantiation_clones_methods";
        cases_run++;
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected concrete variadic instantiations to clone method bodies");
    }

    {
        std::string source =
            "template<typename... Ts>\n"
            "class Box;\n"
            "\n"
            "template<>\n"
            "class Box<> {\n"
            "public:\n"
            "    virtual ~Box() { return; }\n"
            "    int value{};\n"
            "    consteval Box(const char* s) : value{7} { return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<> b{\"hi\"};\n"
            "    return b.get() - 7;\n"
            "}\n";
        std::string case_name = "variadic_empty_pack_base_case_clones_fields";
        cases_run++;
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected the synthesized empty-pack concrete class to keep base-case fields");
    }
}

void run_generic_pack_deduction_tests() {
    {
        std::string case_name = "generic_pack_deduction_substitutes_later_args_into_earlier_function_type";
        cases_run++;
        std::string source =
            "template<typename Sig>\n"
            "class Holder;\n"
            "\n"
            "template<typename R, typename... Params>\n"
            "class Holder<R(Params...)> {\n"
            "public:\n"
            "    virtual ~Holder() { return; }\n"
            "    R (*fn_)(Params...);\n"
            "};\n"
            "\n"
            "int add(int a, int b) { return a + b; }\n"
            "\n"
            "template<typename... Args>\n"
            "int invoke(Holder<int(Args...)>& h, Args&&... args) {\n"
            "    return h.fn_(args...);\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Holder<int(int, int)> h{};\n"
            "    h.fn_ = add;\n"
            "    return invoke(h, 19, 23) - 42;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected later Args... deduction to make the earlier Holder<int(Args...)> "
                                    "parameter type compatible");
    }

    {
        std::string case_name = "generic_pack_deduction_supports_explicit_and_deduced_mixed_arguments";
        cases_run++;
        std::string source =
            "template<typename Sig>\n"
            "class Holder;\n"
            "\n"
            "template<typename R, typename... Params>\n"
            "class Holder<R(Params...)> {\n"
            "public:\n"
            "    virtual ~Holder() { return; }\n"
            "    R (*fn_)(Params...);\n"
            "};\n"
            "\n"
            "int add(int a, int b) { return a + b; }\n"
            "\n"
            "template<typename R, typename... Args>\n"
            "R invoke(Holder<R(Args...)>& h, Args&&... args) {\n"
            "    return h.fn_(args...);\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Holder<int(int, int)> h{};\n"
            "    h.fn_ = add;\n"
            "    return invoke<int>(h, 20, 22) - 42;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected explicit R plus deduced Args... to instantiate successfully");
    }

    {
        std::string case_name = "generic_pack_deduction_rejects_incompatible_earlier_parameter_after_substitution";
        cases_run++;
        std::string source =
            "template<typename Sig>\n"
            "class Holder;\n"
            "\n"
            "template<typename R, typename... Params>\n"
            "class Holder<R(Params...)> {\n"
            "public:\n"
            "    R (*fn_)(Params...);\n"
            "};\n"
            "\n"
            "int add(int a, int b) { return a + b; }\n"
            "\n"
            "template<typename... Args>\n"
            "int invoke(Holder<int(Args...)>& h, Args&&... args) {\n"
            "    return h.fn_(args...);\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Holder<int(int, int)> h;\n"
            "    h.fn_ = add;\n"
            "    return invoke(h, 7);\n"
            "}\n";
        bool threw = monomorphize_pipeline_fails(source);
        expect(threw, case_name + ": expected a mismatched earlier dependent parameter to be rejected");
    }

    {
        std::string case_name = "variadic_generic_parameter_is_checked_after_later_pack_deduction";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<> class Box<> { public: virtual ~Box() { return; } };\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> { public: virtual ~Box() override { return; } };\n"
            "\n"
            "template<typename... Args>\n"
            "int use(const Box<Args...>& fmt, Args&&... args) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Box<int> ok{};\n"
            "    return use(ok, 1) - 42;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected Box<int> plus one later arg to satisfy Box<Args...>");
    }

    {
        std::string case_name = "variadic_generic_parameter_rejects_mismatch_after_later_pack_deduction";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<> class Box<> { public: virtual ~Box() { return; } };\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> { public: virtual ~Box() override { return; } };\n"
            "\n"
            "template<typename... Args>\n"
            "int use(const Box<Args...>& fmt, Args&&... args) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> bad{};\n"
            "    return use(bad, 1) - 42;\n"
            "}\n";
        bool threw = monomorphize_pipeline_fails(source);
        expect(threw, case_name + ": expected Box<int, bool> plus one later arg to be rejected as incompatible with "
                                  "Box<Args...> after Args... deduces to <int>");
    }

    {
        std::string case_name = "variadic_generic_value_parameter_with_converting_ctor_survives_recursive_instantiation";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<>\n"
            "class Box<> {\n"
            "public:\n"
            "    virtual ~Box() { return; }\n"
            "    consteval Box() { return; }\n"
            "    consteval Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    virtual ~Box() override { return; }\n"
            "    consteval Box() { return; }\n"
            "    consteval Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "template<typename... Args>\n"
            "int use(Box<Args...> fmt, Args&&... args) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    return use(\"hi\", 1, true) - 42;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected recursive variadic instantiation to avoid invalidating the active "
                                    "generic function template definition");
    }

    {
        std::string case_name = "variadic_generic_value_parameter_with_converting_ctor_and_fields_codegen";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<>\n"
            "class Box<> {\n"
            "public:\n"
            "    virtual ~Box() { return; }\n"
            "    int value{};\n"
            "    consteval Box() { return; }\n"
            "    consteval Box(const char* s) : value{7} { return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    virtual ~Box() override { return; }\n"
            "    int value{};\n"
            "    consteval Box() { return; }\n"
            "    consteval Box(const char* s) : value{9} { return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "\n"
            "template<typename... Args>\n"
            "int use(Box<Args...> fmt, Args&&... args) {\n"
            "    return fmt.get();\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    return use(\"hi\", 1, true) - 9;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected codegen to use the concrete variadic instantiation rather than the "
                                    "template-shape declaration");
    }
}

void run_generic_function_overload_tests() {
    {
        std::string case_name = "generic_function_overload_by_arity_picks_matching_template";
        cases_run++;
        std::string source =
            "template<typename T>\n"
            "int choose(T x) {\n"
            "    return 1;\n"
            "}\n"
            "\n"
            "template<typename T, typename U>\n"
            "int choose(T x, U y) {\n"
            "    return 2;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    return choose(7) + choose(7, 8) - 3;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected the 1-arg and 2-arg generic overloads to monomorphize independently");
    }

    {
        std::string case_name = "generic_function_overload_can_fall_back_to_nongeneric_helper";
        cases_run++;
        std::string source =
            "int walk(int x) {\n"
            "    return x + 1;\n"
            "}\n"
            "\n"
            "template<typename T>\n"
            "T invoke(T x) {\n"
            "    return walk(x);\n"
            "}\n"
            "\n"
            "template<typename T, typename U>\n"
            "int walk(T x, U y) {\n"
            "    return 0;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    return invoke(1) - 2;\n"
            "}\n";
        bool threw = full_pipeline_fails(source);
        expect(!threw, case_name + ": expected unmatched generic helpers to defer to the nongeneric overload");
    }
}

void run_reference_overload_forwarding_tests() {
    {
        std::string case_name = "reference_typed_local_forwards_to_overloaded_mutable_reference_parameter";
        cases_run++;
        RunResult result = compile_and_run(
            "namespace demo {\n"
            "int f(int a, int& b, int c) {\n"
            "    b = b + a + c;\n"
            "    return b;\n"
            "}\n"
            "int f(int a, int& b) {\n"
            "    return f(a, b, 10);\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    int x = 1;\n"
            "    return demo::f(2, x) - 13;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "reference_typed_local_forwards_to_overloaded_value_parameter";
        cases_run++;
        RunResult result = compile_and_run(
            "namespace demo {\n"
            "int f(int a, int b, int c) {\n"
            "    return a + b + c;\n"
            "}\n"
            "int f(int a, int& b) {\n"
            "    return f(a, b, 10);\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    int x = 1;\n"
            "    return demo::f(2, x) - 13;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "mutable_reference_local_forwards_to_overloaded_const_reference_parameter";
        cases_run++;
        RunResult result = compile_and_run(
            "namespace demo {\n"
            "int f(int a, const int& b, int c) {\n"
            "    return a + b + c;\n"
            "}\n"
            "int f(int a, int& b) {\n"
            "    return f(a, b, 10);\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    int x = 1;\n"
            "    return demo::f(2, x) - 13;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "const_reference_local_does_not_forward_to_overloaded_mutable_reference_parameter";
        cases_run++;
        bool threw = full_pipeline_fails(
            "namespace demo {\n"
            "int f(int a, int& b, int c) {\n"
            "    return a + b + c;\n"
            "}\n"
            "int f(int a, const int& b) {\n"
            "    return f(a, b, 10);\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    int x = 1;\n"
            "    return demo::f(2, x) - 13;\n"
            "}\n");
        expect(threw, case_name + ": expected mutable-reference overload to remain unavailable");
    }
}

void run_functional_tests() {
    std::string case_name = "std_function_rejects_move_only_target";
    cases_run++;
    std::string source =
        "import std;\n"
        "class MoveOnlyAdder {\n"
        "private:\n"
        "    std::unique_ptr<int> value;\n"
        "public:\n"
        "    virtual ~MoveOnlyAdder() = default;\n"
        "    MoveOnlyAdder(std::unique_ptr<int> value) : value{std::move(value)} { return; }\n"
        "    int call(int x) const { return x + *this->value; }\n"
        "};\n"
        "int main() {\n"
        "    std::function<int(int) const> f(MoveOnlyAdder(std::make_unique<int>(5)));\n"
        "    return f(7);\n"
        "}\n";
    bool threw = full_pipeline_fails_with_std_imports(source);
    expect(threw, case_name + ": expected std::function to reject a move-only callable target");
}

void run_thread_tests() {
    std::string case_name = "std_jthread_rejects_reference_capturing_closure";
    cases_run++;
    std::string source =
        "import std;\n"
        "int main() {\n"
        "    int x = 42;\n"
        "    std::jthread t([&x]() { print_int(x); });\n"
        "    return 0;\n"
        "}\n";
    bool threw = full_pipeline_fails_with_std_imports(source);
    expect(threw, case_name + ": expected std::jthread to reject a reference-capturing closure target");
}

void test_compile_time_payload_plan_collects_exported_roots_and_helpers() {
    std::string case_name = "compile_time_payload_plan_collects_exported_roots_and_helpers";
    cases_run++;
    auto program_result = scpp::parse(
        "export module math;\n"
        "namespace math {\n"
        "    class Helper { public: constexpr Helper(int v) : value{v} { return; } int value{}; };\n"
        "    int helper_value(const Helper& h) { return h.value; }\n"
        "    export constexpr int answer() { Helper h{42}; return helper_value(h); }\n"
        "}\n");
    if (!program_result.has_value()) throw std::move(program_result).error();
    scpp::Program program = std::move(program_result.value());
    scpp::CompileTimePayloadPlan plan = scpp::plan_compile_time_payload(program);
    expect(plan.format_version == scpp::SCPPM_COMPILE_TIME_AST_VERSION,
           case_name + ": expected current compile-time payload format version");
    expect(std::find(plan.root_function_names.begin(), plan.root_function_names.end(), "math::answer") !=
               plan.root_function_names.end(),
           case_name + ": expected exported constexpr function root");
    auto reachable_function = [&](std::string_view name) {
        return std::find_if(plan.reachable_function_indices.begin(), plan.reachable_function_indices.end(),
                            [&](std::size_t index) { return index < program.functions.size() && program.functions[index].name == name; }) !=
               plan.reachable_function_indices.end();
    };
    expect(reachable_function("math::helper_value"),
           case_name + ": expected private helper function to be reachable");
    expect(reachable_function("math::Helper_new"),
           case_name + ": expected constexpr constructor to be reachable");
    expect(std::find(plan.reachable_type_names.begin(), plan.reachable_type_names.end(), "math::Helper") !=
               plan.reachable_type_names.end(),
           case_name + ": expected helper type to be reachable");
}

void run_sizeof_tests() {
    {
        std::string case_name = "sizeof_runtime_layout_matches_current_abi_rules";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "sizeof_runtime_layout_matches_current_abi_rules_exe";
        auto compile_result_22 = scpp::compile_to_executable(
            "struct Pair {\n"
            "    char a;\n"
            "    int b;\n"
            "};\n"
            "struct [[scpp::packed]] PackedPair {\n"
            "    char a;\n"
            "    int b;\n"
            "};\n"
            "int main() {\n"
            "    int values[3];\n"
            "    Pair pair{};\n"
            "    if ((int)sizeof(int) != 4) return 1;\n"
            "    if ((int)sizeof(values) != 12) return 2;\n"
            "    if ((int)sizeof(Pair) != 8) return 3;\n"
            "    if ((int)sizeof(pair) != (int)sizeof(Pair)) return 4;\n"
            "    if ((int)sizeof(PackedPair) != 5) return 5;\n"
            "    if ((int)sizeof(&pair) != " +
                std::to_string(sizeof(void*)) +
                ") return 6;\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_22.has_value()) throw std::move(compile_result_22).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected sizeof runtime checks to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "sizeof_is_unevaluated_for_movecheck";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / "sizeof_is_unevaluated_for_movecheck_exe";
        auto compile_result_23 = scpp::compile_to_executable(
            "import std;\n"
            "int consume(std::unique_ptr<int> p) {\n"
            "    return 0;\n"
            "}\n"
            "int main() {\n"
            "    std::unique_ptr<int> p{};\n"
            "    int n = (int)sizeof(std::move(p));\n"
            "    return consume(std::move(p)) + n - n;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_23.has_value()) throw std::move(compile_result_23).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected sizeof operand to be unevaluated, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_can_fold_sizeof_type_and_expr";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_can_fold_sizeof_type_and_expr_exe";
        auto compile_result_24 = scpp::compile_to_executable(
            "struct Tiny {\n"
            "    char x;\n"
            "};\n"
            "consteval int answer() {\n"
            "    Tiny t{};\n"
            "    return (int)sizeof(Tiny) + (int)sizeof(t);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 2;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_24.has_value()) throw std::move(compile_result_24).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval sizeof folding to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_storage_tests() {
    {
        std::string case_name = "alignas_array_storage_uses_max_size_and_alignment";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "alignas_array_storage_uses_max_size_and_alignment_exe";
        auto compile_result_25 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    alignas(int) alignas(long) char slot[sizeof(int) > sizeof(long) ? sizeof(int) : sizeof(long)];\n"
            "    int payload_size() const { return (int)sizeof(this->slot); }\n"
            "};\n"
            "struct Holder {\n"
            "    char tag;\n"
            "    alignas(int) alignas(long) char slot[sizeof(int) > sizeof(long) ? sizeof(int) : sizeof(long)];\n"
            "    char tail;\n"
            "};\n"
            "int main() {\n"
            "    Box box{};\n"
            "    if (box.payload_size() != 8) return 1;\n"
            "    alignas(int) alignas(long) char standalone_slot[sizeof(int) > sizeof(long) ? sizeof(int) : sizeof(long)];\n"
            "    if ((int)sizeof(standalone_slot) != 8) return 2;\n"
            "    if ((int)sizeof(Holder) != 24) return 3;\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_25.has_value()) throw std::move(compile_result_25).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected aligned storage layout checks to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "alignas_array_storage_accepts_user_defined_candidate_types";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "alignas_array_storage_accepts_user_defined_candidate_types_exe";
        auto compile_result_26 = scpp::compile_to_executable(
            "class Widget {\n"
            "public:\n"
            "    virtual ~Widget() = default;\n"
            "    char c;\n"
            "    long value;\n"
            "};\n"
            "struct Wrapper {\n"
            "    char lead;\n"
            "    alignas(Widget) alignas(int) char storage[sizeof(Widget) > sizeof(int) ? sizeof(Widget) : sizeof(int)];\n"
            "};\n"
            "int main() {\n"
            "    alignas(Widget) alignas(int) char storage[sizeof(Widget) > sizeof(int) ? sizeof(Widget) : sizeof(int)];\n"
            "    if ((int)sizeof(storage) != 24) return 1;\n"
            "    if ((int)sizeof(Wrapper) != 32) return 2;\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_26.has_value()) throw std::move(compile_result_26).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected user-defined-type storage checks to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

// ch05 §9.4 (local-constexpr-as-array-bound gap fix): a local `constexpr`
// declared earlier in a function body must be usable as a later local
// array's own bound, exactly like it's already usable as an `alignas`
// operand (run_storage_tests above) and exactly like a *global* `constexpr`
// is already usable as an array bound -- and must otherwise obey ordinary
// C++ block-scoping (no forward reference, no leaking out of a nested
// block, no leaking across sibling functions).
void run_local_constexpr_array_bound_tests() {
    {
        std::string case_name = "local_constexpr_resolves_array_bound_declared_after_it";
        cases_run++;
        RunResult run_result = compile_and_run(
            "int main() {\n"
            "    constexpr int n = 4;\n"
            "    alignas(n) char ok[16]{};\n"
            "    int arr[n];\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < n; i = i + 1) {\n"
            "        arr[i] = i + 1;\n"
            "    }\n"
            "    for (int i = 0; i < n; i = i + 1) {\n"
            "        total = total + arr[i];\n"
            "    }\n"
            "    if ((int)sizeof(arr) != n * (int)sizeof(int)) return 100;\n"
            "    if ((int)sizeof(ok) != 16) return 101;\n"
            "    return total;\n"
            "}\n",
            case_name);
        expect(run_result.exit_code == 10,
               case_name + ": expected local constexpr to resolve both the alignas operand and the later "
                           "array bound, got exit code " +
                   std::to_string(run_result.exit_code));
    }

    {
        std::string case_name = "local_constexpr_array_bound_rejects_use_before_declaration";
        cases_run++;
        auto run_result_22 = try_compile_and_run(
            "int main() {\n"
            "    int arr[n];\n"
            "    constexpr int n = 4;\n"
            "    return 0;\n"
            "}\n",
            case_name);
        bool threw = !run_result_22.has_value();
        std::string message = threw ? run_result_22.error().what() : "";
        expect(threw && message.find("identifier 'n' is not available") != std::string::npos,
               case_name + ": expected a local array bound to reject a constexpr declared later in the same "
                           "function, got message '" +
                   message + "'");
    }

    {
        std::string case_name = "local_constexpr_array_bound_respects_nested_block_scope";
        cases_run++;
        auto run_result_23 = try_compile_and_run(
            "int main() {\n"
            "    {\n"
            "        constexpr int n = 4;\n"
            "    }\n"
            "    int arr[n];\n"
            "    return 0;\n"
            "}\n",
            case_name);
        bool threw = !run_result_23.has_value();
        std::string message = threw ? run_result_23.error().what() : "";
        expect(threw && message.find("identifier 'n' is not available") != std::string::npos,
               case_name + ": expected a nested block's local constexpr to stay out of scope once its own "
                           "block ends, got message '" +
                   message + "'");
    }

    {
        std::string case_name = "local_constexpr_array_bound_is_visible_inside_nested_if_block";
        cases_run++;
        RunResult run_result = compile_and_run(
            "int main() {\n"
            "    constexpr int n = 4;\n"
            "    if (n == 4) {\n"
            "        int arr[n];\n"
            "        arr[n - 1] = 9;\n"
            "        return arr[n - 1];\n"
            "    }\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(run_result.exit_code == 9,
               case_name + ": expected an enclosing local constexpr to stay visible to an array bound inside "
                           "a nested if-block, got exit code " +
                   std::to_string(run_result.exit_code));
    }

    {
        std::string case_name = "local_constexpr_array_bound_does_not_leak_across_sibling_functions";
        cases_run++;
        auto run_result_24 = try_compile_and_run(
            "int main() {\n"
            "    constexpr int n = 4;\n"
            "    int arr[n];\n"
            "    arr[0] = 1;\n"
            "    return arr[0];\n"
            "}\n"
            "void other() {\n"
            "    int leaked[n];\n"
            "}\n",
            case_name);
        bool threw = !run_result_24.has_value();
        std::string message = threw ? run_result_24.error().what() : "";
        expect(threw && message.find("identifier 'n' is not available") != std::string::npos,
               case_name + ": expected a local constexpr in one function to stay invisible to an unrelated "
                           "sibling function, got message '" +
                   message + "'");
    }
}

void run_placement_new_tests() {
    {
        std::string case_name = "placement_new_constructs_scalar_in_storage";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "placement_new_constructs_scalar_in_storage_exe";
        auto compile_result_27 = scpp::compile_to_executable(
            "int main() {\n"
            "    alignas(int) char slot[sizeof(int)]{};\n"
            "    [[scpp::unsafe]] {\n"
            "        int* p = new ((int*)&slot) int(7);\n"
            "        return *p - 7;\n"
            "    }\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_27.has_value()) throw std::move(compile_result_27).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected scalar placement-new path to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "placement_new_constructs_class_in_storage";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "placement_new_constructs_class_in_storage_exe";
        auto compile_result_28 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value{};\n"
            "    Box(int v) : value{v} { return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "int main() {\n"
            "    alignas(Box) char slot[sizeof(Box)]{};\n"
            "    [[scpp::unsafe]] {\n"
            "        Box* p = new ((Box*)&slot) Box(9);\n"
            "        return p->get() - 9;\n"
            "    }\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_28.has_value()) throw std::move(compile_result_28).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected class placement-new path to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_explicit_destructor_tests() {
    {
        std::string case_name = "explicit_destructor_runs_user_declared_destructor";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "explicit_destructor_runs_user_declared_destructor_exe";
        auto compile_result_29 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int* out{};\n"
            "    Box(int* p) : out{p} { return; }\n"
            "    virtual ~Box() { [[scpp::unsafe]] { *this->out = 9; } return; }\n"
            "};\n"
            "int main() {\n"
            "    int result = 0;\n"
            "    alignas(Box) char slot[sizeof(Box)]{};\n"
            "    [[scpp::unsafe]] {\n"
            "        Box* p = new ((Box*)&slot) Box(&result);\n"
            "        p->~Box();\n"
            "    }\n"
            "    return result - 9;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_29.has_value()) throw std::move(compile_result_29).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected explicit destructor call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "object_form_explicit_destructor_is_rejected";
        cases_run++;
        bool threw = full_pipeline_fails(
            "class Box { public: virtual ~Box() { return; } }; int main() { Box b{}; [[scpp::unsafe]] { b.~Box(); } return 0; }");
        expect(threw, case_name + ": expected object-form explicit destructor call to be rejected");
    }
}

void run_consteval_tests() {
    {
        std::string case_name = "consteval_folds_recursive_constexpr_helper";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_folds_recursive_constexpr_helper_exe";
        auto compile_result_30 = scpp::compile_to_executable(
            "constexpr int sum_to(int n) {\n"
            "    if (n == 0) {\n"
            "        return 0;\n"
            "    }\n"
            "    return n + sum_to(n - 1);\n"
            "}\n"
            "consteval int answer() {\n"
            "    return sum_to(6);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 21;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_30.has_value()) throw std::move(compile_result_30).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected folded immediate call to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_constructor_builds_class_object";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_constructor_builds_class_object_exe";
        auto compile_result_31 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value{};\n"
            "    consteval Box(int v) : value{v} { return; }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Box b{42};\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 42;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_31.has_value()) throw std::move(compile_result_31).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval constructor path to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_constructor_implicitly_converts_string_literal_argument";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_constructor_implicitly_converts_string_literal_argument_exe";
        auto compile_result_32 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value{};\n"
            "    consteval Box(const char* text) : value{17} { return; }\n"
            "};\n"
            "int take(Box b) {\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return take(\"hi\") - 17;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_32.has_value()) throw std::move(compile_result_32).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected implicit consteval conversion path to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_constructor_expression_flows_through_consteval_call";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_constructor_expression_flows_through_consteval_call_exe";
        auto compile_result_33 = scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value{};\n"
            "    consteval Box(const char* text) : value{23} { return; }\n"
            "};\n"
            "constexpr int take(Box b) {\n"
            "    return b.value;\n"
            "}\n"
            "consteval int answer() {\n"
            "    return take(Box(\"hi\"));\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 23;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_33.has_value()) throw std::move(compile_result_33).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval constructor expression path to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_helper_call_uses_outer_call_bindings";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_helper_call_uses_outer_call_bindings_exe";
        auto compile_result_34 = scpp::compile_to_executable(
            "consteval int add_40(int x) {\n"
            "    return x + 40;\n"
            "}\n"
            "consteval int route(int x) {\n"
            "    return add_40(x);\n"
            "}\n"
            "int main() {\n"
            "    return route(2) - 42;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_34.has_value()) throw std::move(compile_result_34).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected nested consteval helper call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_constructor_helper_call_accepts_const_char_pointer_parameter";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() /
                                         "consteval_constructor_helper_call_accepts_const_char_pointer_parameter_exe";
        auto compile_result_35 = scpp::compile_to_executable(
            "constexpr int size1(const char* s) {\n"
            "    return 7;\n"
            "}\n"
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value{};\n"
            "    consteval Box(const char* s) : value{size1(s)} { return; }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Box b{\"hi\"};\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 7;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_35.has_value()) throw std::move(compile_result_35).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval constructor helper call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_method_calls_support_mutating_and_const_receivers";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_method_calls_support_mutating_and_const_receivers_exe";
        auto compile_result_36 = scpp::compile_to_executable(
            "class Counter {\n"
            "public:\n"
            "    virtual ~Counter() = default;\n"
            "    int value{};\n"
            "    consteval Counter(int v) : value{v} { return; }\n"
            "    consteval void bump() {\n"
            "        this->value = this->value + 1;\n"
            "        return;\n"
            "    }\n"
            "    constexpr int get() const {\n"
            "        return this->value;\n"
            "    }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Counter c{6};\n"
            "    c.bump();\n"
            "    return c.get();\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 7;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_36.has_value()) throw std::move(compile_result_36).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval/constexpr method calls to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_constructor_local_ctor_call_uses_outer_parameter_bindings";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_constructor_local_ctor_call_uses_outer_parameter_bindings_exe";
        auto compile_result_37 = scpp::compile_to_executable(
            "class Helper {\n"
            "public:\n"
            "    virtual ~Helper() = default;\n"
            "    consteval Helper(const char* s, int i) { return; }\n"
            "};\n"
            "class Outer {\n"
            "public:\n"
            "    virtual ~Outer() = default;\n"
            "    consteval Outer(const char* s) {\n"
            "        Helper h{s, 0};\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "int main() {\n"
            "    Outer o{\"x\"};\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_37.has_value()) throw std::move(compile_result_37).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected local consteval constructor call to use outer ctor bindings, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_helper_call_accepts_derived_object_for_base_parameter";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_helper_call_accepts_derived_object_for_base_parameter_exe";
        auto compile_result_38 = scpp::compile_to_executable(
            "template<typename... Ts> class TagList;\n"
            "template<>\n"
            "class TagList<> {\n"
            "public:\n"
            "    virtual ~TagList() = default;\n"
            "    TagList() { return; }\n"
            "};\n"
            "template<typename Head, typename... Tail>\n"
            "class TagList<Head, Tail...> : private TagList<Tail...> {\n"
            "public:\n"
            "    virtual ~TagList() override = default;\n"
            "    TagList() { return; }\n"
            "};\n"
            "consteval int take(TagList<> tags) {\n"
            "    return 41;\n"
            "}\n"
            "consteval int answer() {\n"
            "    TagList<int, bool> tags{};\n"
            "    return take(tags);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 41;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_38.has_value()) throw std::move(compile_result_38).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected derived-to-base consteval helper call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_helper_call_accepts_derived_object_for_base_reference_parameter";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() /
                                         "consteval_helper_call_accepts_derived_object_for_base_reference_parameter_exe";
        auto compile_result_39 = scpp::compile_to_executable(
            "template<typename... Ts> class TagList;\n"
            "template<>\n"
            "class TagList<> {\n"
            "public:\n"
            "    virtual ~TagList() = default;\n"
            "    TagList() { return; }\n"
            "};\n"
            "template<typename Head, typename... Tail>\n"
            "class TagList<Head, Tail...> : private TagList<Tail...> {\n"
            "public:\n"
            "    virtual ~TagList() override = default;\n"
            "    TagList() { return; }\n"
            "};\n"
            "consteval int take_ref(const TagList<>& tags) {\n"
            "    return 41;\n"
            "}\n"
            "consteval int answer() {\n"
            "    TagList<int, bool> tags{};\n"
            "    return take_ref(tags);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 41;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_39.has_value()) throw std::move(compile_result_39).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected derived-to-base consteval ref call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_rejects_runtime_only_call";
        cases_run++;
        auto compile_result_40 = scpp::compile_to_executable(
            "int runtime_only(int x) {\n"
            "    return x + 1;\n"
            "}\n"
            "consteval int answer() {\n"
            "    return runtime_only(41);\n"
            "}\n"
            "int main() {\n"
            "    return answer();\n"
            "}\n",
            (std::filesystem::current_path() / "consteval_rejects_runtime_only_call_exe").string(),
            std_link_inputs(), prebuilt_module_import_paths());
        bool threw = !compile_result_40.has_value() &&
                     std::string(compile_result_40.error().what()).find(
                         "immediate evaluation may only call constexpr/consteval functions") != std::string::npos;
        expect(threw, case_name + ": expected clear runtime-only immediate-call rejection");
    }

    {
        std::string case_name = "if_consteval_selects_compile_time_and_runtime_branches";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "if_consteval_selects_compile_time_and_runtime_branches_exe";
        auto compile_result_41 = scpp::compile_to_executable(
            "constexpr int choose_positive() {\n"
            "    if consteval {\n"
            "        return 1;\n"
            "    } else {\n"
            "        return 2;\n"
            "    }\n"
            "}\n"
            "constexpr int choose_negative() {\n"
            "    if !consteval {\n"
            "        return 4;\n"
            "    } else {\n"
            "        return 3;\n"
            "    }\n"
            "}\n"
            "consteval int immediate_total() {\n"
            "    return choose_positive() * 10 + choose_negative();\n"
            "}\n"
            "int runtime_total() {\n"
            "    return choose_positive() * 10 + choose_negative();\n"
            "}\n"
            "int main() {\n"
            "    return runtime_total() + immediate_total();\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_41.has_value()) throw std::move(compile_result_41).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 37,
               case_name + ": expected runtime/immediate branch total 37, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_supports_pointer_reads_and_const_spans";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_supports_pointer_reads_and_const_spans_exe";
        auto compile_result_42 = scpp::compile_to_executable(
            "import std;\n"
            "consteval int inspect_views() {\n"
            "    int arr[3];\n"
            "    arr[0] = 4;\n"
            "    arr[1] = 5;\n"
            "    arr[2] = 6;\n"
            "    int* p = &arr[0];\n"
            "    std::span<const int> s = arr;\n"
            "    return *p + s[1] + s.size;\n"
            "}\n"
            "int main() {\n"
            "    return inspect_views();\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_42.has_value()) throw std::move(compile_result_42).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 12,
               case_name + ": expected pointer/span total 12, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "constexpr_local_initializer_is_checked_as_constant_expression";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "constexpr_local_initializer_is_checked_as_constant_expression_exe";
        auto compile_result_43 = scpp::compile_to_executable(
            "constexpr int plus_one(int x) {\n"
            "    return x + 1;\n"
            "}\n"
            "int main() {\n"
            "    constexpr int base = 4;\n"
            "    constexpr int total = plus_one(base);\n"
            "    return total;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_43.has_value()) throw std::move(compile_result_43).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 5,
               case_name + ": expected constexpr local result 5, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "constexpr_local_rejects_runtime_initializer";
        cases_run++;
        auto compile_result_44 = scpp::compile_to_executable(
            "int main() {\n"
            "    int runtime = 4;\n"
            "    constexpr int total = runtime + 1;\n"
            "    return total;\n"
            "}\n",
            (std::filesystem::current_path() / "constexpr_local_rejects_runtime_initializer_exe").string(),
            std_link_inputs(), prebuilt_module_import_paths());
        bool threw = !compile_result_44.has_value() &&
                     std::string(compile_result_44.error().what()).find("identifier 'runtime' is not available") !=
                         std::string::npos;
        expect(threw, case_name + ": expected constexpr local to reject runtime-only initializer");
    }

    {
        std::string case_name = "if_consteval_propagates_required_constant_evaluation_into_callees";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "if_consteval_propagates_required_constant_evaluation_into_callees_exe";
        auto compile_result_45 = scpp::compile_to_executable(
            "consteval int ct_leaf(int x) {\n"
            "    return x + 40;\n"
            "}\n"
            "constexpr int via_if_consteval(int x) {\n"
            "    if consteval {\n"
            "        return ct_leaf(x);\n"
            "    } else {\n"
            "        return x + 1;\n"
            "    }\n"
            "}\n"
            "constexpr int via_if_not_consteval(int x) {\n"
            "    if !consteval {\n"
            "        return x + 2;\n"
            "    } else {\n"
            "        return ct_leaf(x);\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    constexpr int compile_time = via_if_consteval(2) + via_if_not_consteval(2);\n"
            "    int runtime = via_if_consteval(2) + via_if_not_consteval(2);\n"
            "    return compile_time + runtime;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_45.has_value()) throw std::move(compile_result_45).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 91,
               case_name + ": expected required-constant-evaluation total 91, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "if_consteval_skips_non_selected_runtime_only_branch";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "if_consteval_skips_non_selected_runtime_only_branch_exe";
        auto compile_result_46 = scpp::compile_to_executable(
            "int runtime_only(int x) {\n"
            "    return x + 1;\n"
            "}\n"
            "consteval int ct_leaf(int x) {\n"
            "    return x + 40;\n"
            "}\n"
            "constexpr int choose(int x) {\n"
            "    if consteval {\n"
            "        return ct_leaf(x);\n"
            "    } else {\n"
            "        return runtime_only(x);\n"
            "    }\n"
            "}\n"
            "int main() {\n"
            "    constexpr int compile_time = choose(2);\n"
            "    return compile_time + choose(2);\n"
            "}\n",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_46.has_value()) throw std::move(compile_result_46).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 45,
               case_name + ": expected non-selected runtime-only branch to be ignored, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "required_constant_evaluation_rejects_user_defined_destructor_execution";
        cases_run++;
        auto compile_result_47 = scpp::compile_to_executable(
            "class NeedsDrop {\n"
            "public:\n"
            "    int value{};\n"
            "    constexpr NeedsDrop(int x) : value{x} { return; }\n"
            "    virtual ~NeedsDrop() {\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "constexpr int make_value() {\n"
            "    NeedsDrop box{42};\n"
            "    return box.value;\n"
            "}\n"
            "int main() {\n"
            "    constexpr int value = make_value();\n"
            "    return value;\n"
            "}\n",
            (std::filesystem::current_path() /
             "required_constant_evaluation_rejects_user_defined_destructor_execution_exe")
                .string(),
            std_link_inputs(), prebuilt_module_import_paths());
        bool threw = !compile_result_47.has_value() &&
                     std::string(compile_result_47.error().what()).find("cannot execute user-defined destructor of 'NeedsDrop'") !=
                         std::string::npos;
        expect(threw, case_name + ": expected required constant evaluation to reject user-defined destructor execution");
    }
}

void run_cli_extension_tests() {
    {
        std::string case_name = "cli_build_module_emits_roundtrip_artifacts";
        std::filesystem::path root = std::filesystem::current_path() / "cli_build_module_emits_roundtrip_artifacts";
        std::filesystem::path module_source = root / "mymod.scpp";
        std::filesystem::path interface_path = root / "mymod.scppm";
        std::filesystem::path archive_path = root / "libmymod.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module mymod;\n"
                        "namespace mymod {\n"
                        "    export int answer() { return 42; }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        expect(interface_bytes.size() >= 12, case_name + ": expected non-trivial .scppm output");
        if (interface_bytes.size() >= 12) {
            expect(std::string(interface_bytes.begin(), interface_bytes.begin() + 5) == "SCPPM",
                   case_name + ": expected SCPPM magic");
            expect(interface_bytes[5] == 1, case_name + ": expected major version 1");
            expect(interface_bytes[6] == 0, case_name + ": expected patch version 0");
            expect(interface_bytes[7] == 0, case_name + ": expected no generics flag for concrete module");
            std::uint32_t interface_length = read_u32_le(interface_bytes, 8);
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            expect(embedded_source.find("return 42;") == std::string::npos,
                   case_name + ": concrete function body should be stripped from interface source");
            expect(embedded_source.find("export int answer()") != std::string::npos &&
                       embedded_source.find("export int answer() ;") != std::string::npos,
                   case_name + ": concrete function should remain declared in interface source");
        }
        expect(std::filesystem::exists(archive_path), case_name + ": expected .scppa archive to be created");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import mymod;\n"
                        "int main() {\n"
                        "    return mymod::answer() - 42;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                               exe_path.string() + " --import mymod=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": artifact-only consumer build should auto-link the companion libmymod.scppa, got '" +
                  build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected artifact-linked binary to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(archive_path);
        RunResult missing_link_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                               (root / "nolink_app").string() + " --import mymod=" + interface_path.string() + " 2>&1");
        expect(missing_link_result.exit_code != 0,
               case_name + ": build without an available companion libmymod.scppa should fail for a stripped concrete body");
        std::filesystem::remove_all(root);
    }

    // Regression test for a real compiler bug in driver.cppm's
    // hoist_non_partition_imports: a bare `module;` line (the global
    // module fragment opener ch11 §11.2 requires to precede the module
    // declaration) wasn't recognized as belonging with the module
    // declaration below it -- is_module_decl only matches "module "/
    // "export module " with a trailing space before a real name, which a
    // bare "module;" (no space, just a semicolon) never satisfies. It
    // fell through to being treated as ordinary body content instead,
    // and got reordered *after* the hoisted imports in the rendered
    // interface, corrupting the fragment's required leading position.
    // Fixed via a dedicated prologue_lines bucket collected before the
    // module declaration line is seen, re-emitted first, ahead of both
    // the module declaration and any hoisted imports.
    {
        std::string case_name = "leading_global_module_fragment_stays_before_module_declaration";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "gmf_probe.scpp";
        std::filesystem::path interface_path = root / "gmf_probe.scppm";
        std::filesystem::path archive_path = root / "libgmf_probe.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "module;\n"
                        "\n"
                        "export module gmf_probe;\n"
                        "\n"
                        "import std;\n"
                        "\n"
                        "namespace gmf_probe {\n"
                        "    export int value() { return 42; }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module with a leading global module fragment should succeed, got '" +
                   emit_result.stdout_text + "'");
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        if (interface_bytes.size() >= 12) {
            std::uint32_t interface_length = read_u32_le(interface_bytes, 8);
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            std::size_t fragment_pos = embedded_source.find("module;");
            std::size_t decl_pos = embedded_source.find("export module gmf_probe;");
            expect(fragment_pos != std::string::npos,
                   case_name + ": expected the embedded interface to keep the leading 'module;' global module "
                               "fragment, got '" + embedded_source + "'");
            expect(decl_pos != std::string::npos,
                   case_name + ": expected the embedded interface to keep 'export module gmf_probe;', got '" +
                       embedded_source + "'");
            expect(fragment_pos == 0,
                   case_name + ": expected 'module;' to remain the very first line of the rendered interface, "
                               "got '" + embedded_source + "'");
            expect(fragment_pos < decl_pos,
                   case_name + ": expected 'module;' to stay before 'export module gmf_probe;' (not get hoisted "
                               "after it along with imports), got '" + embedded_source + "'");
        } else {
            expect(false, case_name + ": expected a well-formed .scppm interface");
        }
        // Confirm this isn't just a cosmetic text-ordering nicety: a
        // consumer must also be able to actually import and use it.
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        write_text_file(consumer_source,
                        "import gmf_probe;\n"
                        "int main() {\n"
                        "    return gmf_probe::value() - 42;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                               exe_path.string() + " --import gmf_probe=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": a consumer should be able to import the rendered interface, got '" +
                   build_result.stdout_text + "'");
        if (build_result.exit_code == 0) {
            RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
            expect(run_result.exit_code == 0,
                   case_name + ": expected the consumer binary to exit 0, got " +
                       std::to_string(run_result.exit_code));
        }
        std::filesystem::remove_all(root);
    }

    // Regression test for a real compiler bug in driver.cppm's
    // strip_concrete_function_bodies: a nested *local* class defined
    // inside a function body (not a member of the enclosing module's own
    // struct/class) has its own braces, which used to confuse the
    // stripping pass's brace-counting -- it stopped consuming the
    // enclosing function's body one brace too early, leaving the local
    // class's trailing declarations (and everything textually after the
    // enclosing function) shifted/corrupted in the rendered interface.
    // Fixed so the stripper's brace counter treats a nested local class
    // the same as any other nested `{ ... }` block it must skip over as
    // a whole before considering the enclosing function's own body
    // closed.
    {
        std::string case_name = "nested_local_class_body_strips_cleanly_without_corrupting_later_declarations";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "nested.scpp";
        std::filesystem::path interface_path = root / "nested.scppm";
        std::filesystem::path archive_path = root / "libnested.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module test_local_class;\n"
                        "struct Wrapped {\n"
                        "    int value;\n"
                        "    Wrapped(int value) : value{value} {}\n"
                        "};\n"
                        "int outer_function(Wrapped p) {\n"
                        "    struct Inner {\n"
                        "      public:\n"
                        "        Inner(Wrapped w) : w_{w} {}\n"
                        "        int compute() {\n"
                        "            return w_.value;\n"
                        "        }\n"
                        "      private:\n"
                        "        Wrapped w_;\n"
                        "    };\n"
                        "    Inner obj{p};\n"
                        "    return obj.compute();\n"
                        "}\n"
                        "export int call_outer(int v) {\n"
                        "    Wrapped w{v};\n"
                        "    return outer_function(w);\n"
                        "}\n"
                        "export int marker_after() {\n"
                        "    return 999;\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module for a function with a nested local class should succeed, got '" +
                   emit_result.stdout_text + "'");
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        if (interface_bytes.size() >= 12) {
            std::uint32_t interface_length = read_u32_le(interface_bytes, 8);
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            expect(embedded_source.find("struct Inner") == std::string::npos,
                   case_name + ": expected the nested local class to be stripped away entirely along with its "
                               "enclosing function body, got '" + embedded_source + "'");
            expect(embedded_source.find("int outer_function(Wrapped p) ;") != std::string::npos,
                   case_name + ": expected outer_function's body (including its nested local class) to strip "
                               "cleanly to ';', got '" + embedded_source + "'");
            std::size_t outer_pos = embedded_source.find("outer_function");
            std::size_t marker_pos = embedded_source.find("export int marker_after() ;");
            expect(marker_pos != std::string::npos,
                   case_name + ": expected marker_after's declaration to survive uncorrupted after the nested "
                               "local class's enclosing function, got '" + embedded_source + "'");
            expect(outer_pos != std::string::npos && marker_pos != std::string::npos && outer_pos < marker_pos,
                   case_name + ": expected marker_after to still appear after outer_function in the rendered "
                               "interface, got '" + embedded_source + "'");
        } else {
            expect(false, case_name + ": expected a well-formed .scppm interface");
        }
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_roundtrips_fixed_width_builtin_keywords";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    export template<typename T>\n"
                        "    class Box {\n"
                        "    public:\n"
                        "        virtual ~Box() = default;\n"
                        "        T value;\n"
                        "    };\n"
                        "    export Box<std::int64_t> make_box(std::int64_t value) {\n"
                        "        Box<int64_t> box{};\n"
                        "        box.value = value;\n"
                        "        return box;\n"
                        "    }\n"
                        "    export std::int64_t add_one(uint32_t value) {\n"
                        "        return (int64_t)value + 1;\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        expect(std::filesystem::exists(interface_path), case_name + ": expected .scppm output");
        expect(std::filesystem::exists(archive_path), case_name + ": expected .scppa output");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int64_t read_box(const helper::Box<int64_t>& box) { return box.value; }\n"
                        "int main() {\n"
                        "    helper::Box<std::int64_t> lhs = helper::make_box(helper::add_one((std::uint32_t)41));\n"
                        "    return (int)(read_box(lhs) - 42);\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build from .scppm should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected fixed-width consumer binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_partition_roundtrips_without_sources";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_partition_roundtrips_without_sources";
        std::filesystem::path helper_dir = root / "helper";
        std::filesystem::path module_source = root / "partmod.scpp";
        std::filesystem::path partition_source = helper_dir / "partmod_helper.scpp";
        std::filesystem::path interface_path = root / "partmod.scppm";
        std::filesystem::path archive_path = root / "libpartmod.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(helper_dir);
        write_text_file(module_source,
                        "export module partmod;\n"
                        "export import :helper;\n"
                        "namespace partmod {\n"
                        "    export int primary_fn() { return helper_fn() + 1; }\n"
                        "}\n");
        write_text_file(partition_source,
                        "export module partmod:helper;\n"
                        "namespace partmod {\n"
                        "    export int helper_fn() { return 41; }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": partitioned build-module should succeed without self-import workaround, got '" +
                   emit_result.stdout_text + "'");
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        if (interface_bytes.size() >= 12) {
            std::uint32_t interface_length = read_u32_le(interface_bytes, 8);
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            expect(embedded_source.find("export import :helper;") == std::string::npos,
                   case_name + ": merged interface source should not retain partition import directives");
            expect(embedded_source.find("helper_fn") != std::string::npos,
                   case_name + ": merged interface source should include partition declarations");
        }
        std::filesystem::remove(module_source);
        std::filesystem::remove(partition_source);
        write_text_file(consumer_source,
                        "import partmod;\n"
                        "int main() {\n"
                        "    return partmod::primary_fn() + partmod::helper_fn() - 83;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                               exe_path.string() + " --import partmod=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": source-free partition consumer build should auto-link the companion libpartmod.scppa, got '" +
                  build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected partition artifact-linked binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_flat_partition_roundtrips_without_sources";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_flat_partition_roundtrips_without_sources";
        std::filesystem::path module_source = root / "partmod.scpp";
        std::filesystem::path partition_source = root / "helper_any_name.scpp";
        std::filesystem::path interface_path = root / "partmod.scppm";
        std::filesystem::path archive_path = root / "libpartmod.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module partmod;\n"
                        "export import :helper;\n"
                        "namespace partmod {\n"
                        "    export int primary_fn() { return helper_fn() + 1; }\n"
                        "}\n");
        write_text_file(partition_source,
                        "export module partmod:helper;\n"
                        "namespace partmod {\n"
                        "    export int helper_fn() { return 41; }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": flat same-directory partition build-module should succeed, got '" +
                   emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                        "import partmod;\n"
                        "int main() {\n"
                        "    return partmod::primary_fn() + partmod::helper_fn() - 83;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import partmod=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": source-free flat partition consumer build should succeed, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected flat partition artifact-linked binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_exported_type_alias_partition_roundtrips";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_exported_type_alias_partition_roundtrips";
        std::filesystem::path module_source = root / "partmod.scpp";
        std::filesystem::path partition_source = root / "types_any_name.scpp";
        std::filesystem::path interface_path = root / "partmod.scppm";
        std::filesystem::path archive_path = root / "libpartmod.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                       "export module partmod;\n"
                       "export import :types;\n"
                       "namespace partmod {\n"
                       "    export Word plus_one(Word value) { return value + 1; }\n"
                       "}\n");
        write_text_file(partition_source,
                       "export module partmod:types;\n"
                       "namespace partmod {\n"
                       "    export using Word = int;\n"
                       "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                               " --interface-out " + interface_path.string() + " --archive-out " +
                               archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module with exported alias partition should succeed, got '" +
                   emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                       "import partmod;\n"
                       "int main() {\n"
                       "    partmod::Word value = 41;\n"
                       "    return partmod::plus_one(value) - 42;\n"
                       "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                               exe_path.string() + " --import partmod=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer should be able to use imported exported type alias, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected alias-partition artifact-linked binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_generic_payload_roundtrips_without_sources";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_generic_payload_roundtrips_without_sources";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    struct Secret {\n"
                        "        int value;\n"
                        "    };\n"
                        "    template<typename T>\n"
                        "    T add_bonus(T value, const Secret& s) {\n"
                        "        return value + s.value;\n"
                        "    }\n"
                        "    export template<typename T>\n"
                        "    T add_secret(T value) {\n"
                        "        Secret s{};\n"
                        "        s.value = 5;\n"
                        "        return add_bonus(value, s);\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        expect(interface_bytes.size() >= 16, case_name + ": expected payload-bearing .scppm output");
        if (interface_bytes.size() >= 16) {
            expect((interface_bytes[7] & 0x01u) != 0u, case_name + ": expected structured payload flag");
            std::uint32_t interface_length = read_u32_le(interface_bytes, 8);
            expect(interface_bytes.size() >= static_cast<std::size_t>(12 + interface_length + 8),
                   case_name + ": expected payload bytes after embedded interface source");
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            expect(embedded_source.find("return add_bonus(value, s);") == std::string::npos,
                   case_name + ": generic function body should be stripped from interface source");
            expect(embedded_source.find("return value + s.value;") == std::string::npos,
                   case_name + ": private helper generic body should be stripped from interface source");
            std::uint32_t payload_length = read_u32_le(interface_bytes, 12 + interface_length);
            expect(payload_length > 8, case_name + ": expected non-trivial structured payload length");
            if (interface_bytes.size() >= static_cast<std::size_t>(16 + interface_length + payload_length)) {
                std::size_t payload_offset = 16 + interface_length;
                expect(std::string(interface_bytes.begin() + payload_offset, interface_bytes.begin() + payload_offset + 4) ==
                           "SAST",
                       case_name + ": expected structured payload magic");
                expect(read_u32_le(interface_bytes, payload_offset + 4) == scpp::SCPPM_COMPILE_TIME_AST_VERSION,
                       case_name + ": expected current structured payload version");
            }
        }
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    return helper::add_secret(37) - 42;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": generic consumer build should succeed from .scppm payload, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected payload-backed generic binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_roundtrips_const_template_type_args";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_roundtrips_const_template_type_args";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "import std;\n"
                        "namespace helper {\n"
                        "    export using ConstStringPtr = std::unique_ptr<const std::string>;\n"
                        "    export ConstStringPtr make_text() {\n"
                        "        return std::make_unique<const std::string>(\"ok\");\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                        "import std;\n"
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::ConstStringPtr value = helper::make_text();\n"
                        "    return value->length() == 2 ? 0 : 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build from .scppm should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected const-qualified template arg binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_roundtrips_shared_ptr_const_string";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_roundtrips_shared_ptr_const_string";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "import std;\n"
                        "namespace helper {\n"
                        "    export using ConstStringPtr = std::shared_ptr<const std::string>;\n"
                        "    export ConstStringPtr make_text() {\n"
                        "        return std::make_shared<const std::string>(\"ok\");\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                        "import std;\n"
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::ConstStringPtr value = helper::make_text();\n"
                        "    return value->length() == 2 ? 0 : 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build from .scppm should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected shared_ptr const-string binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_prebuilt_variadic_consteval_constructor_and_runtime_method_work";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_prebuilt_variadic_consteval_constructor_and_runtime_method_work";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    export template<typename... Args>\n"
                        "    class Box;\n"
                        "\n"
                        "    export template<>\n"
                        "    class Box<> {\n"
                        "    public:\n"
                        "        virtual ~Box() = default;\n"
                        "        consteval Box() { return; }\n"
                        "        consteval Box(const char* s) { return; }\n"
                        "        int mark() const { return 7; }\n"
                        "    };\n"
                        "\n"
                        "    export template<typename Head, typename... Tail>\n"
                        "    class Box<Head, Tail...> : private helper::Box<Tail...> {\n"
                        "    public:\n"
                        "        virtual ~Box() override = default;\n"
                        "        consteval Box() { return; }\n"
                        "        consteval Box(const char* s) { return; }\n"
                        "        int mark() const { return 11; }\n"
                        "    };\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::Box<int, bool> box{\"ok\"};\n"
                        "    return box.mark() - 11;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": prebuilt variadic generic consumer should succeed, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected prebuilt variadic binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_hidden_class_methods_roundtrips_without_sources";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_hidden_class_methods_roundtrips_without_sources";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    class Holder {\n"
                        "    public:\n"
                        "        virtual ~Holder() = default;\n"
                        "        T value_{};\n"
                        "        Holder(const T& value) : value_{value} { return; }\n"
                        "        T get() const { return this->value_; }\n"
                        "    };\n"
                        "    export template<typename T>\n"
                        "    T roundtrip(const T& value) {\n"
                        "        helper::Holder<T> holder{value};\n"
                        "        return holder.get();\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    return helper::roundtrip(41) - 41;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": hidden class helper consumer build should succeed from .scppm payload, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected hidden-class payload-backed binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_with_exported_generic_class_constructor_hidden_helper_roundtrips";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_build_module_with_exported_generic_class_constructor_hidden_helper_roundtrips";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    class HiddenValidator {\n"
                        "    public:\n"
                        "        virtual ~HiddenValidator() = default;\n"
                        "        consteval HiddenValidator(const char* s) { return; }\n"
                        "    };\n"
                        "    export template<typename T>\n"
                        "    class CheckedString {\n"
                        "    public:\n"
                        "        virtual ~CheckedString() = default;\n"
                        "        const char* text_{};\n"
                        "        consteval CheckedString(const char* s) : text_{s} {\n"
                        "            helper::HiddenValidator<T> validator{s};\n"
                        "            return;\n"
                        "        }\n"
                        "        const char* c_str() const { return this->text_; }\n"
                        "    };\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::CheckedString<int> text{\"ok\"};\n"
                        "    const char* ptr = text.c_str();\n"
                        "    if (ptr[0] == 'o') return 0;\n"
                        "    return 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": exported generic class constructor should keep hidden helper reachable, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected exported generic class constructor payload-backed binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_raw_source_import_keeps_hidden_helper_for_exported_generic_class_constructor";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_raw_source_import_keeps_hidden_helper_for_exported_generic_class_constructor";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    class HiddenValidator {\n"
                        "    public:\n"
                        "        virtual ~HiddenValidator() = default;\n"
                        "        consteval HiddenValidator(const char* s) { return; }\n"
                        "    };\n"
                        "    export template<typename T>\n"
                        "    class CheckedString {\n"
                        "    public:\n"
                        "        virtual ~CheckedString() = default;\n"
                        "        const char* text_{};\n"
                        "        consteval CheckedString(const char* s) : text_{s} {\n"
                        "            helper::HiddenValidator<T> validator{s};\n"
                        "            return;\n"
                        "        }\n"
                        "        const char* c_str() const { return this->text_; }\n"
                        "    };\n"
                        "}\n");
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::CheckedString<int> text{\"ok\"};\n"
                        "    const char* ptr = text.c_str();\n"
                        "    if (ptr[0] == 'o') return 0;\n"
                        "    return 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + module_source.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": raw source import should keep hidden helper reachable, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected raw-source helper binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_private_import_does_not_hide_directly_imported_exported_surface";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_private_import_does_not_hide_directly_imported_exported_surface";
        std::filesystem::path dep_source = root / "dep.scpp";
        std::filesystem::path wrapper_source = root / "wrapper.scpp";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(dep_source,
                        "export module dep;\n"
                        "namespace dep {\n"
                        "    export class Box {\n"
                        "    public:\n"
                        "        virtual ~Box() = default;\n"
                        "        Box() { return; }\n"
                        "        int call() { return 7; }\n"
                        "    };\n"
                        "}\n");
        write_text_file(wrapper_source,
                        "export module wrapper;\n"
                        "import dep;\n"
                        "namespace wrapper {\n"
                        "    export int pass(dep::Box& box) {\n"
                        "        return box.call();\n"
                        "    }\n"
                        "}\n");
        write_text_file(consumer_source,
                        "import dep;\n"
                        "import wrapper;\n"
                        "int main() {\n"
                        "    dep::Box box{};\n"
                        "    if (box.call() != 7) return 1;\n"
                        "    return wrapper::pass(box) - 7;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import dep=" + dep_source.string() +
                                " --import wrapper=" + wrapper_source.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": private importer must not hide directly imported exported members, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected private-import visibility binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name =
            "cli_build_module_with_exported_template_function_value_hidden_helper_roundtrips";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_build_module_with_exported_template_function_value_hidden_helper_roundtrips";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    T hidden_add_one(const T& value) {\n"
                        "        return value + 1;\n"
                        "    }\n"
                        "    template<typename T>\n"
                        "    T apply(T (*fn)(const T&), const T& value) {\n"
                        "        return fn(value);\n"
                        "    }\n"
                        "    export template<typename T>\n"
                        "    T add_one(const T& value) {\n"
                        "        return helper::apply(helper::hidden_add_one<T>, value);\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    return helper::add_one(4) - 5;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": exported template should keep hidden function-value helper reachable, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected hidden function-value payload-backed binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name =
            "cli_build_module_with_hidden_function_value_copy_helper_roundtrips";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_with_hidden_function_value_copy_helper_roundtrips";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    void* hidden_copy(const void* erased) {\n"
                        "        [[scpp::unsafe]] {\n"
                        "            T* raw = static_cast<T*>(erased);\n"
                        "            const T* typed = raw;\n"
                        "            const T& source = *typed;\n"
                        "            return static_cast<void*>(new T(source));\n"
                        "        }\n"
                        "    }\n"
                        "    export template<typename T>\n"
                        "    class Cloner {\n"
                        "    public:\n"
                        "        virtual ~Cloner() = default;\n"
                        "        void* (*copy_)(const void*){};\n"
                        "        Cloner() {\n"
                        "            this->copy_ = helper::hidden_copy<T>;\n"
                        "            return;\n"
                        "        }\n"
                        "        T* clone(const T& value) const {\n"
                        "            [[scpp::unsafe]] {\n"
                        "                void* erased_copy = this->copy_(static_cast<const void*>(&value));\n"
                        "                return static_cast<T*>(erased_copy);\n"
                        "            }\n"
                        "        }\n"
                        "    };\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                                                "import helper;\n"
                                                "class Adder {\n"
                                                "public:\n"
                                                "    virtual ~Adder() { return; }\n"
                                                "    Adder(int& value) : value{value} {\n"
                                                "        return;\n"
                                                "    }\n"
                                                "    int call(int x) const {\n"
                                                "        return x + this->value;\n"
                                                "    }\n"
                                                "private:\n"
                                                "    int& value;\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    int value = 5;\n"
                                                "    Adder base{value};\n"
                                                "    helper::Cloner<Adder> cloner{};\n"
                        "    Adder* copied = cloner.clone(base);\n"
                        "    int result = 0;\n"
                        "    [[scpp::unsafe]] {\n"
                        "        result = copied->call(7);\n"
                        "    }\n"
                        "    return result - 12;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": hidden function-value copy helper should build from .scppm payload, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected hidden copy-helper payload-backed binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_compile_time_dependency_function_is_not_directly_callable";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_compile_time_dependency_function_is_not_directly_callable";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    template<typename T>\n"
                        "    T hidden_add_one(const T& value) {\n"
                        "        return value + 1;\n"
                        "    }\n"
                        "    export template<typename T>\n"
                        "    T add_one(const T& value) {\n"
                        "        return helper::hidden_add_one(value);\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    return helper::hidden_add_one(4) - 5;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code != 0,
               case_name + ": direct call to hidden compile-time dependency should fail");
        expect(build_result.stdout_text.find("no overload of 'helper::hidden_add_one' matches these argument types") !=
                   std::string::npos,
               case_name + ": expected hidden compile-time dependency failure to reject the direct call, got '" +
                   build_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        // Regression test: an exported struct's own public member function must
        // itself be treated as exported (not a hidden compile-time dependency),
        // the inverse of the hidden-function case above. `parse_struct_def` used
        // to compute `def.is_exported` only *after* parsing the struct body, so
        // every member function synthesized while parsing that body observed a
        // stale `is_exported=false` and was hidden even though the struct itself
        // was exported.
        std::string case_name = "cli_exported_struct_member_function_is_directly_callable";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_exported_struct_member_function_is_directly_callable";
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    export struct Counter {\n"
                        "        int value;\n"
                        "        Counter(int value) : value{value} {\n"
                        "            return;\n"
                        "        }\n"
                        "        int add(int amount) const {\n"
                        "            return this->value + amount;\n"
                        "        }\n"
                        "    };\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import helper;\n"
                        "int main() {\n"
                        "    helper::Counter counter{10};\n"
                        "    return counter.add(32) - 42;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": exported struct's public member function should build directly from .scppm payload, "
                           "got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected exported struct member-function-backed binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_allows_local_extern_c_redeclaration_of_hidden_payload_helper";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_build_module_allows_local_extern_c_redeclaration_of_hidden_payload_helper";
        std::filesystem::path base_source = root / "base.scpp";
        std::filesystem::path base_interface = root / "base.scppm";
        std::filesystem::path base_archive = root / "libbase.scppa";
        std::filesystem::path mid_source = root / "mid.scpp";
        std::filesystem::path mid_interface = root / "mid.scppm";
        std::filesystem::path mid_archive = root / "libmid.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(base_source,
                        "export module base;\n"
                        "extern \"C\" {\n"
                        "    void hidden_delete(void* handle);\n"
                        "}\n"
                        "namespace base {\n"
                        "    export template<typename T>\n"
                        "    void touch(const T& value) {\n"
                        "        [[scpp::unsafe]] {\n"
                        "            hidden_delete(nullptr);\n"
                        "        }\n"
                        "        return;\n"
                        "    }\n"
                        "}\n");
        RunResult emit_base =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + base_source.string() +
                                " --interface-out " + base_interface.string() + " --archive-out " +
                                base_archive.string() + " 2>&1");
        expect(emit_base.exit_code == 0,
               case_name + ": base build-module should succeed, got '" + emit_base.stdout_text + "'");
        write_text_file(mid_source,
                        "export module mid;\n"
                        "import base;\n"
                        "extern \"C\" {\n"
                        "    void hidden_delete(void* handle);\n"
                        "}\n"
                        "namespace mid {\n"
                        "    export int ok() {\n"
                        "        return 0;\n"
                        "    }\n"
                        "}\n");
        RunResult emit_mid =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + mid_source.string() +
                                " --interface-out " + mid_interface.string() + " --archive-out " +
                                mid_archive.string() + " --import base=" + base_interface.string() + " 2>&1");
        expect(emit_mid.exit_code == 0,
               case_name + ": importing module should be allowed to redeclare identical extern C helper, got '" +
                   emit_mid.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_rejects_legacy_scppm_missing_structured_payload_for_generic_exports";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_rejects_legacy_scppm_missing_structured_payload_for_generic_exports";
        std::filesystem::path interface_path = root / "legacy.scppm";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_legacy_scppm_without_payload(interface_path,
                                           "export module legacy;\n"
                                           "namespace legacy {\n"
                                           "    export template<typename T>\n"
                                           "    T add_one(T value);\n"
                                           "}\n");
        write_text_file(consumer_source,
                        "import legacy;\n"
                        "int main() {\n"
                        "    return legacy::add_one(1);\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import legacy=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code != 0, case_name + ": expected legacy artifact import to be rejected");
        expect(build_result.stdout_text.find("lacks the required structured compile-time payload") != std::string::npos,
               case_name + ": expected structured payload error, got '" + build_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_rejects_cpp_input";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_rejects_cpp_input.cpp";
        cases_run++;
        write_text_file(source_path, "int main() { return 0; }\n");
        RunResult result = run_command_capture(std::string(SCPP_BINARY_PATH) + " parse " + source_path.string() + " 2>&1");
        std::filesystem::remove(source_path);
        expect(result.exit_code != 0, case_name + ": expected non-zero exit");
        expect(result.stdout_text.find("otherwise pass --source <path>") != std::string::npos,
               case_name + ": expected --source hint, got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "cli_source_flag_builds_cpp_input";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_source_flag_builds_cpp_input.cpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_source_flag_builds_cpp_input_exe";
        cases_run++;
        write_text_file(source_path, "int main() { return 0; }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " --source " +
                                                     source_path.string() + " -o " + exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": expected --source primary input build to succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected built executable to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_rejects_cpp_import_path";
        std::filesystem::path main_path = std::filesystem::current_path() / "cli_rejects_cpp_import_path.scpp";
        std::filesystem::path module_path = std::filesystem::current_path() / "cli_rejects_cpp_import_path_helper.cpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_rejects_cpp_import_path_exe";
        cases_run++;
        write_text_file(main_path, "import helper;\nint main() { return helper::value(); }\n");
        write_text_file(module_path, "export module helper;\nnamespace helper { export int value() { return 1; } }\n");
        RunResult result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + main_path.string() + " -o " +
                                exe_path.string() + " --import helper=" + module_path.string() + " 2>&1");
        std::filesystem::remove(main_path);
        std::filesystem::remove(module_path);
        std::filesystem::remove(exe_path);
        expect(result.exit_code != 0, case_name + ": expected non-zero exit");
        expect(result.stdout_text.find("import path for module 'helper' must use the .scpp or .scppm extension") !=
                   std::string::npos,
               case_name + ": expected import extension error, got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "cli_import_relative_path_diagnostic_uses_as_given_spelling";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_import_relative_path_diagnostic_uses_as_given_spelling";
        std::filesystem::path module_path = root / "lib.scpp";
        std::filesystem::path source_path = root / "main.scpp";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        // Deliberately malformed imported module source so the diagnostic
        // is rooted in lib.scpp itself, not main.scpp.
        write_text_file(module_path,
                        "export module lib;\n"
                        "int helper( {\n");
        write_text_file(source_path, "import lib;\nint main() { return 0; }\n");
        // `cd` into root and use bare relative arguments below (unlike
        // every other test in this file, which prefixes every path with
        // std::filesystem::current_path()) -- the bug this regresses only
        // manifests when --import name=path is given a bare/relative
        // spelling, exactly like an ordinary entry-file argument.
        RunResult build_result = run_command_capture("cd " + root.string() + " && " + std::string(SCPP_BINARY_PATH) +
                                                     " main.scpp -o app --import lib=lib.scpp 2>&1");
        expect(build_result.exit_code != 0, case_name + ": expected malformed imported source to fail the build");
        // Before this fix, a diagnostic rooted in an imported file always
        // printed a fully-resolved absolute path even when --import
        // name=path was given a bare relative one, unlike an equivalent
        // entry-file diagnostic -- see driver.cppm's ModuleCache::resolve.
        expect(build_result.stdout_text.rfind("lib.scpp:", 0) == 0,
               case_name + ": expected diagnostic to start with the as-given relative path 'lib.scpp:', got '" +
                   build_result.stdout_text + "'");
        expect(build_result.stdout_text.find(root.string()) == std::string::npos,
               case_name + ": expected diagnostic to omit the absolute directory entirely, got '" +
                   build_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_import_partition_relative_path_diagnostic_uses_as_given_spelling";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_import_partition_relative_path_diagnostic_uses_as_given_spelling";
        std::filesystem::path partition_dir = root / "part";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(partition_dir);
        write_text_file(root / "mymod.scpp", "export module mymod;\nimport :part;\n");
        // A plain parse error, unconditionally caught while resolving the
        // partition regardless of whether anything in it is ever called.
        write_text_file(partition_dir / "mymod_part.scpp",
                        "module mymod:part;\n"
                        "int broken() {\n"
                        "    return 1 +;\n"
                        "}\n");
        write_text_file(root / "main.scpp", "import mymod;\nint main() { return 0; }\n");
        RunResult build_result = run_command_capture(
            "cd " + root.string() + " && " + std::string(SCPP_BINARY_PATH) +
            " main.scpp -o app --import mymod=mymod.scpp -I . 2>&1");
        expect(build_result.exit_code != 0, case_name + ": expected the partition parse error to fail the build");
        // Same bug as the sibling case above, but for a same-module
        // partition (`import :part;`, resolved by ModuleCache::
        // resolve_partition) inferred from a bare relative --import path
        // rather than an explicit `module:partition=path` mapping.
        expect(build_result.stdout_text.rfind("part/mymod_part.scpp:", 0) == 0,
               case_name +
                   ": expected diagnostic to start with the as-given relative path 'part/mymod_part.scpp:', got '" +
                   build_result.stdout_text + "'");
        expect(build_result.stdout_text.find(root.string()) == std::string::npos,
               case_name + ": expected diagnostic to omit the absolute directory entirely, got '" +
                   build_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_with_I_resolves_module";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_build_with_I_resolves_module.scpp";
        std::filesystem::path module_dir = std::filesystem::current_path() / "cli_build_with_I_resolves_module_dir";
        std::filesystem::path module_path = module_dir / "helper.scpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_build_with_I_resolves_module_exe";
        cases_run++;
        std::filesystem::create_directories(module_dir);
        write_text_file(source_path, "import helper;\nint main() { return helper::value(); }\n");
        write_text_file(module_path, "export module helper;\nnamespace helper { export int value() { return 9; } }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                    " -o " + exe_path.string() + " -I " + module_dir.string() +
                                                    " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": build should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 9,
               case_name + ": expected exit code 9, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(source_path);
        std::filesystem::remove(module_path);
        std::filesystem::remove(exe_path);
        std::filesystem::remove(module_dir);
    }

    {
        std::string case_name = "cli_build_accepts_mixed_positional_and_source_inputs";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_accepts_mixed_positional_and_source_inputs";
        std::filesystem::path source_path = root / "main.scpp";
        std::filesystem::path helper_path = root / "helper.cpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(source_path, "import helper;\nint main() { return helper::value() - 9; }\n");
        write_text_file(helper_path,
                        "export module helper;\n"
                        "namespace helper { export int value() { return 9; } }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " --source " + helper_path.string() + " -o " + exe_path.string() +
                                                     " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": expected mixed positional/--source build to succeed, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected built executable to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_module_source_flag_accepts_cpp_input";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_build_module_source_flag_accepts_cpp_input";
        std::filesystem::path module_path = root / "helper.cpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path source_path = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_path,
                        "export module helper;\n"
                        "namespace helper { export int value() { return 11; } }\n");
        RunResult emit_result = run_command_capture(std::string(SCPP_BINARY_PATH) +
                                                    " build-module --source " + module_path.string() +
                                                    " --interface-out " + interface_path.string() +
                                                    " --archive-out " + archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": expected build-module --source to succeed, got '" + emit_result.stdout_text + "'");
        expect(std::filesystem::exists(interface_path), case_name + ": expected .scppm output");
        expect(std::filesystem::exists(archive_path), case_name + ": expected .scppa output");
        write_text_file(source_path, "import helper;\nint main() { return helper::value() - 11; }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " --import helper=" +
                                                     interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": expected consumer of .cpp-built module to compile, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consumer executable to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_build_prefers_prebuilt_module_over_source";
        std::filesystem::path root = std::filesystem::current_path() / "cli_build_prefers_prebuilt_module_over_source";
        std::filesystem::path module_path = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path source_path = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_path,
                        "export module helper;\n"
                        "namespace helper { export int value() { return 41; } }\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_path.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        write_text_file(module_path,
                        "export module helper;\n"
                        "namespace helper { export int value() { return 99; } }\n");
        write_text_file(source_path, "import helper;\nint main() { return helper::value() - 41; }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " -I " + root.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": build should prefer helper.scppm and auto-link libhelper.scppa, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected .scppm/.scppa to win over helper.scpp, got exit code " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_import_std_works_without_flags";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_import_std_works_without_flags.scpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_import_std_works_without_flags_exe";
        cases_run++;
        write_text_file(source_path,
                       "import std;\n"
                       "int main() {\n"
                       "    std::println(\"{} {}\", std::string(\"hi\"), 2);\n"
                       "    return 2;\n"
                       "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() + " -o " +
                               exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": build should succeed without import flags, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 2,
               case_name + ": expected exit code 2, got " + std::to_string(run_result.exit_code));
        expect(run_result.stdout_text == "hi 2\n",
               case_name + ": expected stdout 'hi 2\n', got '" + run_result.stdout_text + "'");
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_import_std_and_scpp_random_works_without_flags";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_import_std_and_scpp_random_works_without_flags.scpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_import_std_and_scpp_random_works_without_flags_exe";
        cases_run++;
        write_text_file(source_path,
                        "import std;\n"
                        "import scpp;\n"
                        "int main() {\n"
                        "    std::random_device rd{};\n"
                        "    uint32_t expected_max = static_cast<uint32_t>(4294967295);\n"
                        "    if (rd.min() != static_cast<uint32_t>(0)) {\n"
                        "        return 4;\n"
                        "    }\n"
                        "    if (rd.max() != expected_max) {\n"
                        "        return 5;\n"
                        "    }\n"
                        "    std::mt19937 seeded{rd()};\n"
                        "    if (seeded.min() != static_cast<uint32_t>(0)) {\n"
                        "        return 6;\n"
                        "    }\n"
                        "    if (seeded.max() != expected_max) {\n"
                        "        return 7;\n"
                        "    }\n"
                        "    auto hundred = scpp::rand::uniform_int_distribution<int>::make(1, 100);\n"
                        "    if (!hundred.has_value()) {\n"
                        "        return 3;\n"
                        "    }\n"
                        "    int secret = hundred.value()(seeded);\n"
                        "    if (secret < 1 || secret > 100) {\n"
                        "        return 8;\n"
                        "    }\n"
                        "    std::mt19937 gen{123};\n"
                        "    uint32_t first = gen();\n"
                        "    uint32_t second = gen();\n"
                        "    if (first == second) {\n"
                        "        return 1;\n"
                        "    }\n"
                        "    auto die = scpp::rand::uniform_int_distribution<int>::make(1, 6);\n"
                        "    if (!die.has_value()) {\n"
                        "        return 9;\n"
                        "    }\n"
                        "    int roll1 = die.value()(gen);\n"
                        "    int roll2 = die.value()(gen);\n"
                        "    if (roll1 < 1 || roll1 > 6 || roll2 < 1 || roll2 > 6) {\n"
                        "        return 2;\n"
                        "    }\n"
                        "    if (roll1 == roll2 && first == second) {\n"
                        "        return 8;\n"
                        "    }\n"
                        "    return 0;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() + " -o " +
                               exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": build should succeed without import flags, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected exit code 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_import_std_works_after_relocation";
        std::filesystem::path bundle_root = std::filesystem::current_path() / "cli_import_std_works_after_relocation_bundle";
        std::filesystem::path bundle_build_dir = bundle_root / "build";
        std::filesystem::path bundle_build_libs_dir = bundle_build_dir / "libs";
        std::filesystem::path relocated_scpp = bundle_build_dir / "scpp";
        std::filesystem::path source_path = bundle_root / "main.scpp";
        std::filesystem::path exe_path = bundle_root / "app";
        cases_run++;
        std::filesystem::remove_all(bundle_root);
        std::filesystem::create_directories(bundle_build_dir);
        std::filesystem::copy_file(SCPP_BINARY_PATH, relocated_scpp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_BINARY_PATH).parent_path() / "libs", bundle_build_libs_dir,
                              std::filesystem::copy_options::recursive);
        write_text_file(source_path,
                        "import std;\n"
                        "int main() {\n"
                        "    std::string s{\"relocated\"};\n"
                        "    return s.length() == 9 ? 9 : 0;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(relocated_scpp.string() + " " + source_path.string() + " -o " + exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": relocated build should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 9,
               case_name + ": expected relocated binary output exit code 9, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(bundle_root);
    }

    {
        std::string case_name = "cli_import_std_works_from_installed_layout";
        std::filesystem::path install_root = std::filesystem::current_path() / "cli_import_std_works_from_installed_layout_root";
        std::filesystem::path install_bin_dir = install_root / "bin";
        std::filesystem::path install_libs_dir = install_root / "share" / "scpp" / "libs";
        std::filesystem::path installed_scpp = install_bin_dir / "scpp";
        std::filesystem::path source_path = install_root / "main.scpp";
        std::filesystem::path exe_path = install_root / "app";
        cases_run++;
        std::filesystem::remove_all(install_root);
        std::filesystem::create_directories(install_bin_dir);
        std::filesystem::create_directories(install_libs_dir);
        std::filesystem::copy_file(SCPP_BINARY_PATH, installed_scpp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_STDLIB_STD_MODULE_PATH).parent_path(), install_libs_dir,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_BINARY_PATH).parent_path() / "libs", install_libs_dir,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        write_text_file(source_path,
                        "import std;\n"
                        "int main() {\n"
                        "    const char* tail = \"ok\";\n"
                        "    std::print(\"{} {}\", std::string(\"installed\"), tail);\n"
                        "    std::println(\"{{ready}}\");\n"
                        "    return 9;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(installed_scpp.string() + " " + source_path.string() + " -o " + exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": installed-layout build should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 9,
               case_name + ": expected installed-layout binary output exit code 9, got " +
                   std::to_string(run_result.exit_code));
        expect(run_result.stdout_text == "installed ok{ready}\n",
               case_name + ": expected installed-layout stdout 'installed ok{ready}\n', got '" +
                   run_result.stdout_text + "'");
        std::filesystem::remove_all(install_root);
    }

    {
        std::string case_name = "cli_g_emits_debug_sections";
        std::filesystem::path source_path = std::filesystem::current_path() / "cli_g_emits_debug_sections.scpp";
        std::filesystem::path exe_path = std::filesystem::current_path() / "cli_g_emits_debug_sections_exe";
        cases_run++;
        write_text_file(source_path,
                        "int add(int x, int y) {\n"
                        "    int sum = x + y;\n"
                        "    return sum;\n"
                        "}\n"
                        "int main() {\n"
                        "    return add(2, 5);\n"
                        "}\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " -g 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": debug build should succeed, got '" + build_result.stdout_text + "'");
        RunResult readelf_result = run_command_capture("readelf -S " + exe_path.string() + " 2>&1");
        expect(readelf_result.exit_code == 0,
               case_name + ": readelf should succeed, got '" + readelf_result.stdout_text + "'");
        expect(readelf_result.stdout_text.find(".debug_info") != std::string::npos,
               case_name + ": expected .debug_info section, got '" + readelf_result.stdout_text + "'");
        expect(readelf_result.stdout_text.find(".debug_line") != std::string::npos,
               case_name + ": expected .debug_line section, got '" + readelf_result.stdout_text + "'");
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_g_nested_scope_local_has_dwarf_location";
        std::filesystem::path source_path =
            std::filesystem::current_path() / "cli_g_nested_scope_local_has_dwarf_location.scpp";
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "cli_g_nested_scope_local_has_dwarf_location_exe";
        cases_run++;
        write_text_file(source_path,
                        "int identity(int n) {\n"
                        "    if (n <= 1) {\n"
                        "        return 1;\n"
                        "    }\n"
                        "    int copy = n + 1;\n"
                        "    return copy;\n"
                        "}\n"
                        "int main() {\n"
                        "    return identity(5) - 6;\n"
                        "}\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " -g 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": debug build should succeed, got '" + build_result.stdout_text + "'");
        if (build_result.exit_code == 0) expect_dwarf_variable_has_location(exe_path, "copy", case_name);
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_g_partition_function_uses_partition_source_file";
        std::filesystem::path source_root =
            std::filesystem::current_path() / "cli_g_partition_function_uses_partition_source_file";
        std::filesystem::path helper_dir = source_root / "helper";
        std::filesystem::path module_path = source_root / "mymod.scpp";
        std::filesystem::path helper_path = helper_dir / "mymod_helper.scpp";
        std::filesystem::path main_path = source_root / "main.scpp";
        std::filesystem::path exe_path = source_root / "mainbin";
        cases_run++;
        std::filesystem::remove_all(source_root);
        std::filesystem::create_directories(helper_dir);
        write_text_file(module_path, "export module mymod;\nexport import :helper;\n");
        write_text_file(helper_path,
                        "export module mymod:helper;\n"
                        "namespace mymod {\n"
                        "    export int compute(int x) {\n"
                        "        int doubled = x * 2;\n"
                        "        return doubled;\n"
                        "    }\n"
                        "}\n");
        write_text_file(main_path,
                        "import mymod;\n"
                        "int main() {\n"
                        "    int result = mymod::compute(21);\n"
                        "    return result - 42;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + main_path.string() + " -o " +
                                exe_path.string() + " -g --import mymod=" + module_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": debug build should succeed, got '" + build_result.stdout_text + "'");
        if (build_result.exit_code == 0) {
            expect_dwarf_named_entry_contains(exe_path, "DW_TAG_subprogram", "mymod::compute",
                                              helper_path.string(), case_name);
            expect_dwarf_named_entry_contains(exe_path, "DW_TAG_variable", "doubled",
                                              helper_path.string(), case_name);
        }
        std::filesystem::remove_all(source_root);
    }

    {
        std::string case_name = "cli_g_deeply_nested_local_has_dwarf_location";
        std::filesystem::path source_path =
            std::filesystem::current_path() / "cli_g_deeply_nested_local_has_dwarf_location.scpp";
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "cli_g_deeply_nested_local_has_dwarf_location_exe";
        cases_run++;
        write_text_file(source_path,
                        "int sum_until(int n) {\n"
                        "    int total = 0;\n"
                        "    while (n > 0) {\n"
                        "        if (n > 1) {\n"
                        "            int copy = n + 1;\n"
                        "            total = total + copy;\n"
                        "        }\n"
                        "        n = n - 1;\n"
                        "    }\n"
                        "    return total;\n"
                        "}\n"
                        "int main() {\n"
                        "    return sum_until(3) - 7;\n"
                        "}\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " -g 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": debug build should succeed, got '" + build_result.stdout_text + "'");
        if (build_result.exit_code == 0) expect_dwarf_variable_has_location(exe_path, "copy", case_name);
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "cli_import_module_scans_declared_name_not_filename";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_import_module_scans_declared_name_not_filename";
        std::filesystem::path lib_dir = root / "lib";
        std::filesystem::path module_source = lib_dir / "named_anything.scpp";
        std::filesystem::path main_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(lib_dir);
        write_text_file(module_source,
                        "export module helper;\n"
                        "namespace helper {\n"
                        "    export int answer() { return 42; }\n"
                        "}\n");
        write_text_file(main_source,
                        "import helper;\n"
                        "int main() { return helper::answer() - 42; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) +
                                                     " main.scpp -o app -I lib 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": content-based module discovery under -I should succeed, got '" +
                   build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected discovered-module binary to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_compile_type_aliases_are_transparent";
        std::filesystem::path root = std::filesystem::current_path() / "cli_compile_type_aliases_are_transparent";
        std::filesystem::path main_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(main_source,
                        "using Word = int;\n"
                        "using WordRef = Word&;\n"
                        "using WordPtr = Word*;\n"
                        "Word add(WordRef lhs, Word rhs) {\n"
                        "    return lhs + rhs;\n"
                        "}\n"
                        "int main() {\n"
                        "    alignas(Word) Word x = 20;\n"
                        "    Word y = 21;\n"
                        "    WordPtr ptr = &y;\n"
                        "    Word sum = add(x, y);\n"
                        "    return sizeof(Word) == sizeof(int) ? sum - 41 : 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + main_source.string() + " -o " +
                                exe_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": direct compile with aliases should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected alias-using executable to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_project_build_builds_manifest_bin";
        std::filesystem::path root = std::filesystem::current_path() / "cli_project_build_builds_manifest_bin";
        std::filesystem::path src_dir = root / "src";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "hello";
        std::filesystem::path helper_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "modules" / "helper.scppm";
        std::filesystem::path helper_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "archives" / "libhelper.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(src_dir);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"hello\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"hello\"\n"
                        "sources = [\"src/**/*.scpp\"]\n");
        write_text_file(src_dir / "helper.scpp",
                        "export module helper;\n"
                        "namespace helper { export int value() { return 42; } }\n");
        write_text_file(src_dir / "main.scpp",
                        "import helper;\n"
                        "int main() { return helper::value() - 42; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": scpp build should succeed, got '" + build_result.stdout_text + "'");
        expect(std::filesystem::exists(exe_path), case_name + ": expected manifest-built executable");
        expect(std::filesystem::exists(helper_iface), case_name + ": expected helper .scppm output");
        expect(std::filesystem::exists(helper_archive), case_name + ": expected helper .scppa output");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected manifest-built executable to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_project_build_builds_manifest_bin_with_cpp_named_sources";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_project_build_builds_manifest_bin_with_cpp_named_sources";
        std::filesystem::path src_dir = root / "src";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "hello";
        std::filesystem::path helper_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "modules" / "helper.scppm";
        std::filesystem::path helper_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "hello" / "archives" / "libhelper.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(src_dir);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"hello\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"hello\"\n"
                        "sources = [\"src/**/*.cpp\"]\n");
        write_text_file(src_dir / "helper.cpp",
                        "export module helper;\n"
                        "namespace helper { export int value() { return 42; } }\n");
        write_text_file(src_dir / "main.cpp",
                        "import helper;\n"
                        "int main() { return helper::value() - 42; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": manifest build with .cpp-named sources should succeed, got '" +
                   build_result.stdout_text + "'");
        expect(std::filesystem::exists(exe_path), case_name + ": expected manifest-built executable");
        expect(std::filesystem::exists(helper_iface), case_name + ": expected helper .scppm output");
        expect(std::filesystem::exists(helper_archive), case_name + ": expected helper .scppa output");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected manifest-built executable to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_project_build_lib_with_flat_partition_layout_succeeds";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_project_build_lib_with_flat_partition_layout_succeeds";
        std::filesystem::path iface_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "demo" / "modules" / "demo.scppm";
        std::filesystem::path archive_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "demo" / "archives" / "libdemo.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"demo\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"demo\"\n"
                        "sources = [\"**/*.scpp\"]\n");
        write_text_file(root / "demo.scpp",
                        "export module demo;\n"
                        "export import :part;\n"
                        "namespace demo {\n"
                        "    export int primary() { return answer() + 1; }\n"
                        "}\n");
        write_text_file(root / "part.scpp",
                        "export module demo:part;\n"
                        "namespace demo {\n"
                        "    export int answer() { return 41; }\n"
                        "}\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build --lib 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": flat same-directory partition package build should succeed, got '" +
                   build_result.stdout_text + "'");
        expect(std::filesystem::exists(iface_path), case_name + ": expected manifest-built demo interface");
        expect(std::filesystem::exists(archive_path), case_name + ": expected manifest-built demo archive");
        write_text_file(consumer_source,
                        "import demo;\n"
                        "int main() { return demo::primary() + demo::answer() - 83; }\n");
        RunResult consumer_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                       shell_quote(SCPP_BINARY_PATH) + " main.scpp -o app --import demo=" +
                                                       shell_quote(iface_path.string()) + " 2>&1");
        expect(consumer_build.exit_code == 0,
               case_name + ": expected consumer of flat-layout built package to compile, got '" +
                   consumer_build.stdout_text + "'");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected flat-layout package consumer to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_project_build_bare_scpp_aliases_build";
        std::filesystem::path root = std::filesystem::current_path() / "cli_project_build_bare_scpp_aliases_build";
        std::filesystem::path src_dir = root / "src";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "app" / "app";
        std::filesystem::path lib_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "app" / "modules" / "mylib.scppm";
        std::filesystem::path lib_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "app" / "archives" / "libmylib.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(src_dir);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"app\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"mylib\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"app\"\n"
                        "sources = [\"src/**/*.scpp\"]\n");
        write_text_file(src_dir / "mylib.scpp",
                        "export module mylib;\n"
                        "namespace mylib { export int answer() { return 42; } }\n");
        write_text_file(src_dir / "main.scpp",
                        "import mylib;\n"
                        "int main() { return mylib::answer() - 42; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": bare scpp should build the manifest project, got '" + build_result.stdout_text + "'");
        expect(std::filesystem::exists(exe_path), case_name + ": expected package executable output");
        expect(std::filesystem::exists(lib_iface), case_name + ": expected library interface output");
        expect(std::filesystem::exists(lib_archive), case_name + ": expected library archive output");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected bare-scpp project executable to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_workspace_build_enforces_direct_visibility_and_links_transitively";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_workspace_build_enforces_direct_visibility_and_links_transitively";
        std::filesystem::path tls_dir = root / "tls";
        std::filesystem::path net_dir = root / "net";
        std::filesystem::path app_dir = root / "app";
        std::filesystem::path app_exe =
            root / ".scpp" / "build" / scpp::host_target_triple() / "app" / "app";
        std::filesystem::path tls_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "tls" / "archives" / "libtls.scppa";
        std::filesystem::path net_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "net" / "archives" / "libnet.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(tls_dir / "src");
        std::filesystem::create_directories(net_dir / "src");
        std::filesystem::create_directories(app_dir / "src");
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[workspace]\n"
                        "members = [\"tls\", \"net\", \"app\"]\n"
                        "default-members = [\"app\"]\n");
        write_text_file(tls_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"tls\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"tls\"\n"
                        "sources = [\"src/**/*.scpp\"]\n");
        write_text_file(tls_dir / "src" / "tls.scpp",
                        "export module tls;\n"
                        "namespace tls { export int seed() { return 40; } }\n");
        write_text_file(net_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"net\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"net\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "tls = { path = \"../tls\" }\n");
        write_text_file(net_dir / "src" / "net.scpp",
                        "export module net;\n"
                        "import tls;\n"
                        "namespace net { export int value() { return tls::seed() + 2; } }\n");
        write_text_file(app_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"app\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"app\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "net = { path = \"../net\" }\n");
        write_text_file(app_dir / "src" / "main.scpp",
                        "import net;\n"
                        "int main() { return net::value() - 42; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": workspace default build should succeed, got '" + build_result.stdout_text + "'");
        expect(std::filesystem::exists(app_exe), case_name + ": expected workspace app executable");
        expect(std::filesystem::exists(tls_archive), case_name + ": expected tls archive at workspace root output");
        expect(std::filesystem::exists(net_archive), case_name + ": expected net archive at workspace root output");
        expect(!std::filesystem::exists(app_dir / ".scpp"),
               case_name + ": member packages should not write outputs under their own directories");
        RunResult run_result = run_command_capture(shell_quote(app_exe.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected app executable exit code 0, got " + std::to_string(run_result.exit_code));
        RunResult package_build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                             shell_quote(SCPP_BINARY_PATH) + " build -p net --lib 2>&1");
        expect(package_build_result.exit_code == 0,
               case_name + ": package-selected lib build should succeed, got '" + package_build_result.stdout_text + "'");
        RunResult workspace_build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                               shell_quote(SCPP_BINARY_PATH) + " build --workspace 2>&1");
        expect(workspace_build_result.exit_code == 0,
               case_name + ": --workspace build should succeed, got '" + workspace_build_result.stdout_text + "'");
        write_text_file(app_dir / "src" / "main.scpp",
                        "import tls;\n"
                        "int main() { return tls::seed(); }\n");
        RunResult direct_visibility_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                                 shell_quote(SCPP_BINARY_PATH) + " build -p app 2>&1");
        expect(direct_visibility_result.exit_code != 0,
               case_name + ": importing a transitive-only dependency should fail");
        expect(direct_visibility_result.stdout_text.find(
                   "module 'tls' is exported only by transitive dependency package 'tls'") != std::string::npos,
               case_name + ": expected direct-visibility error, got '" + direct_visibility_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_root_package_workspace_builds_root_package_by_default";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_root_package_workspace_builds_root_package_by_default";
        std::filesystem::path dep_dir = root / "libs" / "tls";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "rootapp" / "rootapp";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(dep_dir / "src");
        std::filesystem::create_directories(root / "src");
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[workspace]\n"
                        "members = [\"libs/tls\"]\n"
                        "\n"
                        "[package]\n"
                        "name = \"rootapp\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"rootapp\"\n"
                                                "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "tls = { path = \"libs/tls\" }\n");
        write_text_file(dep_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"tls\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"tls\"\n"
                        "sources = [\"src/**/*.scpp\"]\n");
        write_text_file(dep_dir / "src" / "tls.scpp",
                        "export module tls;\n"
                        "namespace tls { export int seed() { return 5; } }\n");
        write_text_file(root / "src" / "main.scpp",
                        "import tls;\n"
                        "int main() { return tls::seed() - 5; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": root package workspace build should succeed, got '" + build_result.stdout_text + "'");
        expect(std::filesystem::exists(exe_path), case_name + ": expected root package executable at workspace root");
        expect(!std::filesystem::exists(dep_dir / ".scpp"),
               case_name + ": workspace member outputs should remain under the workspace root");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected root package executable exit code 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_incremental_build_skips_recompile_on_impl_change_and_recompiles_on_interface_change";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_incremental_build_skips_recompile_on_impl_change_and_recompiles_on_interface_change";
        std::filesystem::path dep_dir = root / "dep";
        std::filesystem::path app_dir = root / "app";
        std::filesystem::path build_root = root / ".scpp" / "build" / scpp::host_target_triple();
        std::filesystem::path dep_archive = build_root / "dep" / "archives" / "libdep.scppa";
        std::filesystem::path dep_interface = build_root / "dep" / "modules" / "dep.scppm";
        std::filesystem::path app_object = build_root / "app" / "objects" / "app" / "0_main_scpp.o";
        std::filesystem::path app_exe = build_root / "app" / "app";
        std::filesystem::path build_db = root / ".scpp" / "cache" / "build.db";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(dep_dir / "src");
        std::filesystem::create_directories(app_dir / "src");
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[workspace]\n"
                        "members = [\"dep\", \"app\"]\n"
                        "default-members = [\"app\"]\n");
        write_text_file(dep_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"dep\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"dep\"\n"
                        "sources = [\"src/**/*.scpp\"]\n");
        write_text_file(dep_dir / "src" / "dep.scpp",
                        "export module dep;\n"
                        "namespace dep {\n"
                        "    int internal_value() {\n"
                        "        int base = 39;\n"
                        "        return base + 1;\n"
                        "    }\n"
                        "    export int value() { return internal_value(); }\n"
                        "}\n");
        write_text_file(app_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"app\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"app\"\n"
                                                "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "dep = { path = \"../dep\" }\n");
        write_text_file(app_dir / "src" / "main.scpp",
                        "import dep;\n"
                        "int main() { return dep::value() - 40; }\n");
        RunResult first_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                    shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(first_build.exit_code == 0, case_name + ": initial build should succeed, got '" + first_build.stdout_text + "'");
        expect(std::filesystem::exists(build_db), case_name + ": expected .scpp/cache/build.db");
        auto first_dep_archive_time = std::filesystem::last_write_time(dep_archive);
        std::string first_dep_interface_text = read_file(dep_interface);
        auto first_app_object_time = std::filesystem::last_write_time(app_object);
        auto first_app_exe_time = std::filesystem::last_write_time(app_exe);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        write_text_file(dep_dir / "src" / "dep.scpp",
                        "export module dep;\n"
                        "namespace dep {\n"
                        "    int internal_value() {\n"
                        "        int base = 38;\n"
                        "        return base + 2;\n"
                        "    }\n"
                        "    export int value() { return internal_value(); }\n"
                        "}\n");
        RunResult impl_only_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                        shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(impl_only_build.exit_code == 0,
               case_name + ": impl-only rebuild should succeed, got '" + impl_only_build.stdout_text + "'");
        expect(std::filesystem::last_write_time(dep_archive) > first_dep_archive_time,
               case_name + ": dependency archive should rebuild after implementation change");
        expect(read_file(dep_interface) == first_dep_interface_text,
               case_name + ": dependency interface should remain unchanged after implementation-only change");
        expect(std::filesystem::last_write_time(app_object) == first_app_object_time,
               case_name + ": downstream object should be reused when dependency interface is unchanged");
        expect(std::filesystem::last_write_time(app_exe) > first_app_exe_time,
               case_name + ": final executable should relink after dependency archive changes");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        write_text_file(dep_dir / "src" / "dep.scpp",
                        "export module dep;\n"
                        "namespace dep {\n"
                        "    export int extra() { return 0; }\n"
                        "    int internal_value() {\n"
                        "        int base = 38;\n"
                        "        return base + 2;\n"
                        "    }\n"
                        "    export int value() { return internal_value(); }\n"
                        "}\n");
        RunResult interface_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                        shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(interface_build.exit_code == 0,
               case_name + ": interface rebuild should succeed, got '" + interface_build.stdout_text + "'");
        expect(read_file(dep_interface) != first_dep_interface_text,
               case_name + ": dependency interface should rebuild after exported interface change");
        expect(std::filesystem::last_write_time(app_object) > first_app_object_time,
               case_name + ": downstream object should recompile after dependency interface change");
        RunResult run_result = run_command_capture(shell_quote(app_exe.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": executable should still run after incremental rebuilds, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        // Regression test: a primary interface module's build-cache signature
        // used to hash only the primary source file itself, not any interface
        // partitions it re-exports via `export import :part;`. That meant
        // editing ONLY a partition file left the cached signature unchanged,
        // so `scpp build` incorrectly treated the module/archive as already
        // up to date and never recompiled it, silently keeping stale behavior.
        std::string case_name = "cli_project_build_lib_rebuilds_when_only_partition_file_changes";
        std::filesystem::path root =
            std::filesystem::current_path() / "cli_project_build_lib_rebuilds_when_only_partition_file_changes";
        std::filesystem::path iface_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "demo" / "modules" / "demo.scppm";
        std::filesystem::path archive_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "demo" / "archives" / "libdemo.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"demo\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"demo\"\n"
                        "sources = [\"**/*.scpp\"]\n");
        write_text_file(root / "demo.scpp",
                        "export module demo;\n"
                        "export import :part;\n"
                        "namespace demo {\n"
                        "    export int primary() { return answer() + 1; }\n"
                        "}\n");
        write_text_file(root / "part.scpp",
                        "export module demo:part;\n"
                        "namespace demo {\n"
                        "    export int answer() { return 41; }\n"
                        "}\n");
        RunResult first_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                    shell_quote(SCPP_BINARY_PATH) + " build --lib 2>&1");
        expect(first_build.exit_code == 0,
               case_name + ": initial build should succeed, got '" + first_build.stdout_text + "'");
        auto first_archive_time = std::filesystem::last_write_time(archive_path);
        write_text_file(consumer_source,
                        "import demo;\n"
                        "int main() { return demo::answer(); }\n");
        RunResult first_consumer_build =
            run_command_capture("cd " + shell_quote(root.string()) + " && " + shell_quote(SCPP_BINARY_PATH) +
                                " main.scpp -o app --import demo=" + shell_quote(iface_path.string()) + " 2>&1");
        expect(first_consumer_build.exit_code == 0,
               case_name + ": expected initial consumer build to succeed, got '" +
                   first_consumer_build.stdout_text + "'");
        RunResult first_run = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(first_run.exit_code == 41,
               case_name + ": expected initial partition value 41, got " + std::to_string(first_run.exit_code));
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        write_text_file(root / "part.scpp",
                        "export module demo:part;\n"
                        "namespace demo {\n"
                        "    export int answer() { return 42; }\n"
                        "}\n");
        RunResult second_build = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build --lib 2>&1");
        expect(second_build.exit_code == 0,
               case_name + ": rebuild after partition-only change should succeed, got '" +
                   second_build.stdout_text + "'");
        expect(std::filesystem::last_write_time(archive_path) > first_archive_time,
               case_name + ": expected archive to rebuild after partition-only source change");
        RunResult second_consumer_build =
            run_command_capture("cd " + shell_quote(root.string()) + " && " + shell_quote(SCPP_BINARY_PATH) +
                                " main.scpp -o app --import demo=" + shell_quote(iface_path.string()) + " 2>&1");
        expect(second_consumer_build.exit_code == 0,
               case_name + ": expected second consumer build to succeed, got '" +
                   second_consumer_build.stdout_text + "'");
        RunResult second_run = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(second_run.exit_code == 42,
               case_name + ": expected updated partition value 42 after partition-only rebuild, got " +
                   std::to_string(second_run.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        // Regression test: a [[lib]] target may bundle multiple independent
        // primary modules (not partitions of one another) into a single
        // build, each still producing its own interface/archive artifact.
        // Archive naming falls back to being keyed by module name (rather
        // than the shared target name) specifically to avoid two modules'
        // archives colliding on the same path.
        std::string case_name = "cli_project_build_lib_supports_multiple_independent_primary_modules";
        std::filesystem::path root = std::filesystem::current_path() /
                                     "cli_project_build_lib_supports_multiple_independent_primary_modules";
        std::filesystem::path modone_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "multilib" / "modules" / "modone.scppm";
        std::filesystem::path modtwo_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "multilib" / "modules" / "modtwo.scppm";
        std::filesystem::path modone_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "multilib" / "archives" / "libmodone.scppa";
        std::filesystem::path modtwo_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "multilib" / "archives" / "libmodtwo.scppa";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"multilib\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"multilib\"\n"
                        "sources = [\"*.scpp\"]\n");
        write_text_file(root / "modone.scpp",
                        "export module modone;\n"
                        "namespace modone {\n"
                        "    export int value_one() { return 10; }\n"
                        "}\n");
        write_text_file(root / "modtwo.scpp",
                        "export module modtwo;\n"
                        "namespace modtwo {\n"
                        "    export int value_two() { return 20; }\n"
                        "}\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build --lib 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": build of a [[lib]] target with two independent primary modules should succeed, got '" +
                   build_result.stdout_text + "'");
        expect(std::filesystem::exists(modone_iface), case_name + ": expected modone interface artifact");
        expect(std::filesystem::exists(modtwo_iface), case_name + ": expected modtwo interface artifact");
        expect(std::filesystem::exists(modone_archive),
               case_name + ": expected modone archive named after its own module, not the shared target");
        expect(std::filesystem::exists(modtwo_archive),
               case_name + ": expected modtwo archive named after its own module, not the shared target");
        write_text_file(root / "main.scpp",
                        "import modone;\n"
                        "import modtwo;\n"
                        "int main() { return modone::value_one() + modtwo::value_two() - 30; }\n");
        RunResult consumer_build =
            run_command_capture("cd " + shell_quote(root.string()) + " && " + shell_quote(SCPP_BINARY_PATH) +
                                " main.scpp -o app --import modone=" + shell_quote(modone_iface.string()) +
                                " --import modtwo=" + shell_quote(modtwo_iface.string()) + " 2>&1");
        expect(consumer_build.exit_code == 0,
               case_name + ": expected consumer importing both modules to build, got '" +
                   consumer_build.stdout_text + "'");
        RunResult run_result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected both modules' archives to link and run correctly (10 + 20 - 30 == 0), got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        // Regression test: the exactly-one-primary-module restriction is
        // still enforced when a [[lib]] target also has additional_objs
        // configured, since build_library_target's native-object merge has
        // to pick a single archive to merge into and can't do so
        // unambiguously when the target builds more than one module.
        std::string case_name =
            "cli_project_build_lib_rejects_multiple_primary_modules_with_additional_objs";
        std::filesystem::path root =
            std::filesystem::current_path() /
            "cli_project_build_lib_rejects_multiple_primary_modules_with_additional_objs";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"multilib\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"multilib\"\n"
                        "sources = [\"*.scpp\"]\n"
                        "additional_objs = \"custom\"\n"
                        "\n"
                        "[additional_objs.custom]\n"
                        "input = [\"native.cpp\"]\n"
                        "output = [\"native.o\"]\n"
                        "command = \"\"\"\\\n"
                        "${CXX:-c++} -std=c++26 -O2 -c native.cpp\n"
                        "\"\"\"\n");
        write_text_file(root / "native.cpp", "int scpp_multilib_native_marker() { return 7; }\n");
        write_text_file(root / "modone.scpp",
                        "export module modone;\n"
                        "namespace modone {\n"
                        "    export int value_one() { return 10; }\n"
                        "}\n");
        write_text_file(root / "modtwo.scpp",
                        "export module modtwo;\n"
                        "namespace modtwo {\n"
                        "    export int value_two() { return 20; }\n"
                        "}\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build --lib 2>&1");
        expect(build_result.exit_code != 0,
               case_name + ": expected a [[lib]] target with two primary modules and additional_objs to be rejected");
        expect(build_result.stdout_text.find(
                   "must contain exactly one primary interface module when using additional_objs") !=
                   std::string::npos,
               case_name + ": expected the additional_objs-specific error, got '" + build_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_native_links_propagate_transitively";
        std::filesystem::path root = std::filesystem::current_path() / "cli_native_links_propagate_transitively";
        std::filesystem::path trig_dir = root / "trig";
        std::filesystem::path net_dir = root / "net";
        std::filesystem::path app_dir = root / "app";
        std::filesystem::path app_exe =
            root / ".scpp" / "build" / scpp::host_target_triple() / "app" / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(trig_dir / "src");
        std::filesystem::create_directories(net_dir / "src");
        std::filesystem::create_directories(app_dir / "src");
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[workspace]\n"
                        "members = [\"trig\", \"net\", \"app\"]\n"
                        "default-members = [\"app\"]\n");
        write_text_file(trig_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"trig\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"trig\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[native]\n"
                        "links = [\"m\"]\n");
        write_text_file(trig_dir / "src" / "trig.scpp",
                        "export module trig;\n"
                        "extern \"C\" double cos(double x);\n"
                        "namespace trig {\n"
                        "    export int one() {\n"
                        "        [[scpp::unsafe]] { return (int)cos(0.0); }\n"
                        "    }\n"
                        "}\n");
        write_text_file(net_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"net\"\n"
                        "\n"
                        "[[lib]]\n"
                        "name = \"net\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "trig = { path = \"../trig\" }\n");
        write_text_file(net_dir / "src" / "net.scpp",
                        "export module net;\n"
                        "import trig;\n"
                        "namespace net { export int forward() { return trig::one(); } }\n");
        write_text_file(app_dir / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"app\"\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"app\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[dependencies]\n"
                        "net = { path = \"../net\" }\n");
        write_text_file(app_dir / "src" / "main.scpp",
                        "import net;\n"
                        "int main() { return net::forward() - 1; }\n");
        RunResult build_result = run_command_capture("cd " + shell_quote(root.string()) + " && " +
                                                     shell_quote(SCPP_BINARY_PATH) + " build 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": transitive native-link build should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(shell_quote(app_exe.string()) + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": executable should run successfully with propagated native links, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }

    {
        std::string case_name = "cli_static_build_produces_self_contained_binary";
        std::filesystem::path source_path =
            std::filesystem::current_path() / "cli_static_build_produces_self_contained_binary.scpp";
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "cli_static_build_produces_self_contained_binary_exe";
        cases_run++;
        write_text_file(source_path, "int main() { return 7; }\n");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " --static 2>&1");
        expect(build_result.exit_code == 0, case_name + ": static build should succeed, got '" +
                                                build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 7, case_name + ": expected static binary exit code 7, got " +
                                             std::to_string(run_result.exit_code));
        RunResult ldd_result = run_command_capture("ldd " + exe_path.string() + " 2>&1");
        expect(ldd_result.stdout_text.find("not a dynamic executable") != std::string::npos ||
                   ldd_result.stdout_text.find("statically linked") != std::string::npos,
               case_name + ": expected ldd to report a fully static binary, got '" + ldd_result.stdout_text + "'");
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }
}

void run_enum_tests() {
    {
        std::string case_name = "enum_cast_returns_expected_value_for_declared_enumerator";
        cases_run++;
        RunResult result = compile_and_run(
            "import std;\n"
            "import scpp;\n"
            "enum class Color : uint8_t { red = 1, green = 2, blue = 3 };\n"
            "int main() {\n"
            "    auto color = scpp::enum_cast<Color>((uint8_t)2);\n"
            "    if (!color.has_value()) return 1;\n"
            "    if (color.value() != Color::green) return 2;\n"
            "    uint8_t raw = static_cast<uint8_t>(Color::blue);\n"
            "    return (int)raw - 3;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "enum_cast_returns_invalid_value_error_for_unknown_enumerator";
        cases_run++;
        RunResult result = compile_and_run(
            "import std;\n"
            "import scpp;\n"
            "enum class Color : uint8_t { red = 1, green = 2, blue = 3 };\n"
            "int main() {\n"
            "    auto color = scpp::enum_cast<Color>((uint8_t)9);\n"
            "    if (color.has_value()) return 1;\n"
            "    return color.error() == scpp::enum_cast_error::invalid_value ? 0 : 2;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "enum_class_cross_type_comparison_is_rejected";
        cases_run++;
        bool threw = full_pipeline_fails(
            "enum class Color { red };\n"
            "enum class Shape { red };\n"
            "int main() { return Color::red == Shape::red; }\n");
        expect(threw, case_name + ": expected cross-enum comparison to be rejected");
    }

    {
        std::string case_name = "enum_class_explicit_int_to_enum_cast_is_rejected";
        cases_run++;
        bool threw = [&] {
            auto program_result = scpp::parse(
                "enum class Color { red };\n"
                "int main() { Color color = static_cast<Color>(1); return 0; }\n");
            if (!program_result.has_value()) return true;
            scpp::Program program = std::move(program_result.value());
            auto monomorphize_result = scpp::monomorphize_generics(program);
            if (!monomorphize_result.has_value()) {
                expect(std::string(monomorphize_result.error().what()).find("scpp::enum_cast<Color>(value)") !=
                           std::string::npos,
                       case_name + ": expected enum_cast guidance in move-check diagnostic");
                return true;
            }
            auto check_moves_result = scpp::check_moves(program);
            if (!check_moves_result.has_value()) {
                expect(std::string(check_moves_result.error().what()).find("scpp::enum_cast<Color>(value)") !=
                           std::string::npos,
                       case_name + ": expected enum_cast guidance in move-check diagnostic");
                return true;
            }
            scpp::Codegen codegen("test_module");
            auto generate_result = codegen.generate(program);
            if (!generate_result.has_value()) {
                expect(std::string(generate_result.error().what()).find("scpp::enum_cast<Color>(value)") !=
                           std::string::npos,
                       case_name + ": expected enum_cast guidance in codegen diagnostic");
                return true;
            }
            return false;
        }();
        expect(threw, case_name + ": expected explicit int-to-enum cast to be rejected");
    }

    {
        std::string case_name = "enum_class_implicit_enum_to_int_is_rejected";
        cases_run++;
        bool threw = full_pipeline_fails(
            "enum class Color { red };\n"
            "int main() { int value = Color::red; return value; }\n");
        expect(threw, case_name + ": expected implicit enum-to-int conversion to be rejected");
    }

    {
        std::string case_name = "enum_class_module_import_round_trip_works";
        cases_run++;
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::path module_path = root / "colors.scpp";
        std::filesystem::path exe_path = root / "enum_import_exe";
        write_text_file(module_path,
                        "export module colors;\n"
                        "namespace colors {\n"
                        "    export enum class Color : uint8_t { red = 1, green = 2 };\n"
                        "    export Color favorite() { return colors::Color::green; }\n"
                        "}\n");
        auto compile_result_48 = scpp::compile_to_executable(
            "import colors;\n"
            "int main() {\n"
            "    uint8_t lhs = static_cast<uint8_t>(colors::favorite());\n"
            "    uint8_t rhs = static_cast<uint8_t>(colors::Color::green);\n"
            "    return (int)lhs - (int)rhs;\n"
            "}\n",
            exe_path.string(), {}, {{"colors", module_path.string()}});
        if (!compile_result_48.has_value()) throw std::move(compile_result_48).error();
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected imported enum module executable to succeed, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }
}

void run_switch_tests() {
    {
        std::string case_name = "switch_non_empty_case_requires_explicit_terminator";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / (case_name + "_exe");
        std::filesystem::remove(exe_path);
        auto compile_result_49 = scpp::compile_to_executable(
            "int main() {\n"
            "    int value = 0;\n"
            "    switch (1) {\n"
            "        case 1:\n"
            "            value = 1;\n"
            "        case 2:\n"
            "            value = 2;\n"
            "        default:\n"
            "            return value;\n"
            "    }\n"
            "}\n",
            exe_path.string(), std_link_inputs(),
            prebuilt_module_import_paths());
        bool threw = !compile_result_49.has_value();
        if (threw) {
            expect(std::string(compile_result_49.error().what()).find("must end with 'break;'") != std::string::npos,
                   case_name + ": expected explicit-terminator diagnostic");
        }
        std::filesystem::remove(exe_path);
        expect(threw, case_name + ": expected compile-time rejection");
    }

    {
        std::string case_name = "switch_fallthrough_attribute_allows_continuation";
        cases_run++;
        RunResult result = compile_and_run(
            "int main() {\n"
            "    int value = 0;\n"
            "    switch (1) {\n"
            "        case 1:\n"
            "            value = 1;\n"
            "            [[fallthrough]];\n"
            "        case 2:\n"
            "            value = value + 2;\n"
            "            break;\n"
            "        default:\n"
            "            value = value + 4;\n"
            "            break;\n"
            "    }\n"
            "    return value - 3;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "switch_on_enum_type_executes_matching_case";
        cases_run++;
        RunResult result = compile_and_run(
            "enum class Color { red = 1, green = 2, blue = 3 };\n"
            "int main() {\n"
            "    Color color = Color::green;\n"
            "    switch (color) {\n"
            "        case Color::red:\n"
            "            return 1;\n"
            "        case Color::green:\n"
            "            return 0;\n"
            "        default:\n"
            "            return 2;\n"
            "    }\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "switch_grouped_case_labels_share_following_body";
        cases_run++;
        RunResult result = compile_and_run(
            "int main() {\n"
            "    int value = 0;\n"
            "    switch (2) {\n"
            "        case 1:\n"
            "        case 2:\n"
            "            value = 7;\n"
            "            break;\n"
            "        default:\n"
            "            return 1;\n"
            "    }\n"
            "    return value - 7;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "switch_duplicate_case_values_are_rejected";
        cases_run++;
        auto program_result = scpp::parse(
            "int main() {\n"
            "    switch (1) {\n"
            "        case 1:\n"
            "            return 1;\n"
            "        case 1:\n"
            "            return 0;\n"
            "    }\n"
            "}\n");
        bool threw = false;
        if (program_result.has_value()) {
            scpp::Program program = std::move(program_result.value());
            auto error_message = monomorphize_or_check_moves_error_message(program);
            if (error_message.has_value()) {
                threw = true;
                expect(error_message->find("duplicate switch case value") != std::string::npos,
                       case_name + ": expected duplicate-case diagnostic");
            }
        }
        expect(threw, case_name + ": expected a DataflowError");
    }

    {
        std::string case_name = "switch_on_non_integral_type_is_rejected";
        cases_run++;
        auto program_result = scpp::parse(
            "struct Box { int value; };\n"
            "int main() {\n"
            "    Box box{1};\n"
            "    switch (box) {\n"
            "        default:\n"
            "            return 0;\n"
            "    }\n"
            "}\n");
        bool threw = false;
        if (program_result.has_value()) {
            scpp::Program program = std::move(program_result.value());
            auto error_message = monomorphize_or_check_moves_error_message(program);
            if (error_message.has_value()) {
                threw = true;
                expect(error_message->find("integral or enum") != std::string::npos,
                       case_name + ": expected integral-or-enum diagnostic");
            }
        }
        expect(threw, case_name + ": expected a DataflowError");
    }
}

void run_global_scope_resolution_tests() {
    {
        std::string case_name = "global_scope_resolution_bypasses_namespace_shadowing";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
        auto compile_result_50 = scpp::compile_to_executable(
            "int ping() { return 41; }\n"
            "namespace inner {\n"
            "int ping() {\n"
            "    return ::ping() + 1;\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    return inner::ping() - 42;\n"
            "}\n",
            exe_path.string());
        if (!compile_result_50.has_value()) throw std::move(compile_result_50).error();
        RunResult result = run_command_capture(exe_path.string() + " 2>&1");
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "global_scope_resolution_bypasses_std_namespace_shadowing";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
        auto compile_result_51 = scpp::compile_to_executable(
            "int raw_ping() { return 41; }\n"
            "namespace std {\n"
            "int raw_ping() {\n"
            "    return ::raw_ping() + 1;\n"
            "}\n"
            "}\n"
            "int main() {\n"
            "    return std::raw_ping() - 42;\n"
            "}\n",
            exe_path.string());
        if (!compile_result_51.has_value()) throw std::move(compile_result_51).error();
        RunResult result = run_command_capture(exe_path.string() + " 2>&1");
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_nodiscard_tests() {
    {
        std::string case_name = "nodiscard_function_discard_is_rejected";
        cases_run++;
        auto compile_result_52 = scpp::compile_to_executable(
            "[[nodiscard]] int answer() { return 7; }\n"
            "int main() {\n"
            "    answer();\n"
            "    return 0;\n"
            "}\n",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_52.has_value() &&
                     std::string(compile_result_52.error().what()).find("nodiscard function 'answer'") != std::string::npos;
        expect(threw, case_name + ": expected nodiscard discard diagnostic");
    }

    {
        std::string case_name = "nodiscard_reason_is_included_in_diagnostic";
        cases_run++;
        auto compile_result_53 = scpp::compile_to_executable(
            "[[nodiscard(\"check the status\")]] int answer() { return 7; }\n"
            "int main() {\n"
            "    answer();\n"
            "    return 0;\n"
            "}\n",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_53.has_value() &&
                     std::string(compile_result_53.error().what()).find("check the status") != std::string::npos;
        expect(threw, case_name + ": expected nodiscard reason in diagnostic");
    }

    {
        std::string case_name = "nodiscard_type_propagates_to_returning_function";
        cases_run++;
        auto compile_result_54 = scpp::compile_to_executable(
            "struct [[nodiscard(\"keep the status\")]] status {\n"
            "    int code;\n"
            "};\n"
            "status make_status() {\n"
            "    status s{};\n"
            "    s.code = 5;\n"
            "    return s;\n"
            "}\n"
            "int main() {\n"
            "    make_status();\n"
            "    return 0;\n"
            "}\n",
            (std::filesystem::current_path() / case_name).string());
        bool threw = false;
        if (!compile_result_54.has_value()) {
            std::string message = compile_result_54.error().what();
            threw = message.find("nodiscard type 'status'") != std::string::npos &&
                    message.find("keep the status") != std::string::npos;
        }
        expect(threw, case_name + ": expected nodiscard type diagnostic");
    }

    {
        std::string case_name = "using_nodiscard_results_is_allowed";
        cases_run++;
        RunResult result = compile_and_run(
            "[[nodiscard]] int answer() { return 7; }\n"
            "int twice(int value) { return value * 2; }\n"
            "struct [[nodiscard]] status {\n"
            "    int code;\n"
            "};\n"
            "status make_status() {\n"
            "    status s{};\n"
            "    s.code = 5;\n"
            "    return s;\n"
            "}\n"
            "int forward_answer() { return answer(); }\n"
            "status forward_status() { return make_status(); }\n"
            "int main() {\n"
            "    int a = answer();\n"
            "    int b = twice(answer());\n"
            "    status s = forward_status();\n"
            "    return a + b + s.code + forward_answer() - 33;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

}

void run_static_member_function_tests() {
    {
        std::string case_name = "static_member_function_is_callable_via_class_qualification";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(class Math {
public:
    virtual ~Math() { return; }
    static int add_one(int value) { return value + 1; }
};
int main() {
    return Math::add_one(6) - 7;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "static_member_function_can_use_private_constructor_and_field";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(class Box {
public:
                virtual ~Box() = default;
                static int reveal(int value) {
                    Box box{value};
                    return box.secret;
    }
private:
    int secret{};
    Box(int value) : secret{value} { return; }
};
int main() {
    return Box::reveal(9) - 9;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "static_member_function_on_template_specialization_is_callable";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(template<typename T>
class Box;

template<>
class Box<int> {
public:
    virtual ~Box() { return; }
    static Box<int> make(int value) {
        Box<int> box{value};
        return box;
    }

    int reveal() const {
        return this->secret;
    }

private:
    int secret{};

    Box(int value) : secret{value} { return; }
};

int main() {
    Box<int> box = Box<int>::make(9);
    return box.reveal() - 9;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "private_constructor_is_not_callable_outside_the_class";
        cases_run++;
        auto compile_result_55 = scpp::compile_to_executable(
            R"SCPP(class Box {
public:
    virtual ~Box() = default;
    static int reveal(int value) {
        Box box{value};
        return box.secret;
    }
private:
    int secret{};
    Box(int value) : secret{value} { return; }
};
int main() {
    Box box{4};
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_55.has_value() &&
                     std::string(compile_result_55.error().what()).find("private constructor") != std::string::npos;
        expect(threw, case_name + ": expected private constructor diagnostic");
    }

    {
        std::string case_name = "static_member_function_cannot_use_this";
        cases_run++;
        auto compile_result_56 = scpp::compile_to_executable(
            R"SCPP(class Box {
public:
    virtual ~Box() = default;
    int secret;
    static int broken() {
        return this->secret;
    }
};
int main() { return 0; }
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_56.has_value() &&
                     std::string(compile_result_56.error().what()).find("undeclared variable 'this'") != std::string::npos;
        expect(threw, case_name + ": expected static method to reject use of this");
    }
}

void run_default_argument_tests() {
    {
        std::string case_name = "default_argument_literals_and_multiple_trailing_omissions_work";
        RunResult result = compile_and_run(
            "int sum3(int a, int b = 2, int c = 3) {\n"
            "    return a + b + c;\n"
            "}\n"
            "int main() {\n"
            "    return sum3(1) + sum3(1, 4) + sum3(1, 4, 5) - 24;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "default_argument_expression_is_evaluated_at_each_call_site";
        RunResult result = compile_and_run(
            "int next_value() {\n"
            "    static int counter = 40;\n"
            "    counter = counter + 1;\n"
            "    return counter;\n"
            "}\n"
            "int take(int value = next_value()) {\n"
            "    return value;\n"
            "}\n"
            "int main() {\n"
            "    return take() + take() - 83;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "default_argument_empty_braces_value_initializes_parameter";
        RunResult result = compile_and_run(
            "int zero(int value = {}) {\n"
            "    return value;\n"
            "}\n"
            "int main() {\n"
            "    return zero();\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "generic_function_default_argument_works";
        RunResult result = compile_and_run(
            "template<typename T>\n"
            "T add_default(T value, int extra = 1) {\n"
            "    return value + extra;\n"
            "}\n"
            "int main() {\n"
            "    return add_default(41) - 42;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "constructor_default_argument_works";
        RunResult result = compile_and_run(
            "class Box {\n"
            "public:\n"
            "    int value{};\n"
            "    Box(int x = 7) : value{x} { return; }\n"
            "    virtual ~Box() { return; }\n"
            "};\n"
            "int main() {\n"
            "    Box b{};\n"
            "    return b.value - 7;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "default_argument_trailing_rule_is_rejected";
        auto compile_result_57 = scpp::compile_to_executable(
            "int bad(int x = 1, int y) { return x + y; }\n"
            "int main() { return bad(1, 2); }\n",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_57.has_value() &&
                     std::string(compile_result_57.error().what()).find("every later parameter must also have one") !=
                         std::string::npos;
        expect(threw, case_name + ": expected trailing-only default-argument diagnostic");
    }
}

void run_static_local_lifetime_tests() {
    {
        std::string case_name = "returning_reference_to_static_local_is_accepted";
        RunResult result = compile_and_run(
            "class Holder {\n"
            "public:\n"
            "    const int& stable_value() const {\n"
            "        static int value = 42;\n"
            "        return value;\n"
            "    }\n"
            "    virtual ~Holder() = default;\n"
            "};\n"
            "int main() {\n"
            "    Holder holder{};\n"
            "    return holder.stable_value() - 42;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "returning_reference_to_non_static_local_is_still_rejected";
        cases_run++;
        auto compile_result_58 = scpp::compile_to_executable(
            R"SCPP(class Holder {
public:
    const int& dangling_value() const {
        int value = 42;
        return value;
    }
    virtual ~Holder() = default;
};
int main() {
    Holder holder{};
    return holder.dangling_value();
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_58.has_value() &&
                     std::string(compile_result_58.error().what()).find("returns a reference derived from 'value'") !=
                         std::string::npos;
        expect(threw, case_name + ": expected ordinary local reference return to remain rejected");
    }
}

void run_loop_reborrow_release_tests() {
    {
        std::string case_name = "shared_reborrow_inside_loop_releases_each_iteration";
        cases_run++;
        RunResult result = compile_and_run(
            "struct Item {\n"
            "    int value;\n"
            "};\n"
            "struct Holder {\n"
            "    Item item;\n"
            "};\n"
            "int sum(const Holder& holder, int count) {\n"
            "    int total = 0;\n"
            "    for (int i = 0; i < count; i++) {\n"
            "        const Item& current = holder.item;\n"
            "        total += current.value;\n"
            "    }\n"
            "    return total;\n"
            "}\n"
            "int main() {\n"
            "    Holder holder{};\n"
            "    holder.item.value = 3;\n"
            "    return sum(holder, 2) == 6 ? 0 : 1;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_implicit_member_field_access_tests() {
    {
        std::string case_name = "member_shared_ptr_deref_without_explicit_this_can_return_reference";
        cases_run++;
        RunResult result = compile_and_run(
            "import std;\n"
            "class Holder {\n"
            "public:\n"
            "    std::shared_ptr<const std::string> text{};\n"
            "    Holder() {\n"
            "        this->text = std::make_shared<const std::string>(\"ok\");\n"
            "        return;\n"
            "    }\n"
            "    const std::string& text_ref() const {\n"
            "        return text.operator*();\n"
            "    }\n"
            "    virtual ~Holder() = default;\n"
            "};\n"
            "int main() {\n"
            "    Holder holder{};\n"
            "    return holder.text_ref().length() == 2 ? 0 : 1;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "member_optional_deref_without_explicit_this_compiles";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / case_name;
        auto compile_result_59 = scpp::compile_to_executable(
            R"SCPP(import std;
class Holder {
public:
    std::optional<std::string> text{};
    const std::string& text_ref() const {
        return text.operator*();
    }
    virtual ~Holder() = default;
};
int main() {
    return 0;
}
)SCPP",
            exe_path.string());
        bool threw = !compile_result_59.has_value();
        expect(!threw, case_name + ": expected optional member deref to compile without explicit this");
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "local_optional_deref_still_cannot_escape";
        cases_run++;
        auto compile_result_60 = scpp::compile_to_executable(
            R"SCPP(import std;
class Holder {
public:
    const std::string& broken() const {
        std::optional<std::string> text{};
        return text.operator*();
    }
    virtual ~Holder() = default;
};
int main() {
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_60.has_value() &&
                     std::string(compile_result_60.error().what()).find("returns a reference derived from 'text'") !=
                         std::string::npos;
        expect(threw, case_name + ": expected local optional deref escape to remain rejected");
    }
}

void run_random_tests() {
    {
        std::string case_name = "scpp_rand_uniform_int_distribution_rejects_empty_range";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int main() {
    auto bad = scpp::rand::uniform_int_distribution<int>::make(9, 3);
    if (bad.has_value()) return 1;
    if (bad.error() != scpp::rand::error::empty_range) return 2;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "scpp_rand_uniform_int_distribution_produces_working_distribution";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int main() {
    auto maybe_die = scpp::rand::uniform_int_distribution<int>::make(1, 6);
    if (!maybe_die.has_value()) return 1;
    std::mt19937 gen{123};
    int roll1 = maybe_die.value()(gen);
    int roll2 = maybe_die.value()(gen);
    if (roll1 < 1 || roll1 > 6) return 2;
    if (roll2 < 1 || roll2 > 6) return 3;
    auto maybe_singleton = scpp::rand::uniform_int_distribution<int>::make(4, 4);
    if (!maybe_singleton.has_value()) return 4;
    if (maybe_singleton.value()(gen) != 4) return 5;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "scpp_rand_uniform_int_rand_returns_zero_for_non_positive_max";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int main() {
    if (scpp::rand::uniform_int_rand(0) != 0) return 1;
    if (scpp::rand::uniform_int_rand(-7) != 0) return 2;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "scpp_rand_uniform_int_rand_stays_within_half_open_range";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int main() {
    int max_value = 7;
    int i = 0;
    while (i < 200) {
        int value = scpp::rand::uniform_int_rand(max_value);
        if (value < 0) return 1;
        if (value >= max_value) return 2;
        i = i + 1;
    }
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "scpp_rand_uniform_int_distribution_direct_constructor_is_not_public";
        std::filesystem::path source_path = std::filesystem::current_path() / (case_name + ".scpp");
        std::filesystem::path exe_path = std::filesystem::current_path() / (case_name + "_exe");
        cases_run++;
        write_text_file(source_path,
                        R"SCPP(import std;
import scpp;
int main() {
    scpp::rand::uniform_int_distribution<int> die(1, 6);
    std::mt19937 gen{123};
    return die(gen);
}
)SCPP");
        RunResult build_result = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + source_path.string() +
                                                     " -o " + exe_path.string() + " 2>&1");
        expect(build_result.exit_code != 0,
               case_name + ": direct construction should be rejected, got '" + build_result.stdout_text + "'");
        expect(build_result.stdout_text.find("uniform_int_distribution") != std::string::npos,
               case_name + ": expected constructor diagnostic to mention uniform_int_distribution, got '" +
                    build_result.stdout_text + "'");
        std::filesystem::remove(source_path);
        std::filesystem::remove(exe_path);
    }
}

void run_vector_tests() {
    {
        std::string case_name = "std_vector_operator_index_aborts_out_of_bounds";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::vector<int> values{};
    values.push_back(1);
    return values[1];
}
)SCPP",
            case_name);
        expect(result.exit_code != 0,
               case_name + ": expected non-zero exit code, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_vector_at_aborts_out_of_bounds";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::vector<int> values{};
    values.push_back(1);
    return values.at((std::size_t)-1);
}
)SCPP",
            case_name);
        expect(result.exit_code != 0,
               case_name + ": expected non-zero exit code, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "build_module_roundtrips_std_vector_api";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "vectorlib.scpp";
        std::filesystem::path interface_path = root / "vectorlib.scppm";
        std::filesystem::path archive_path = root / "libvectorlib.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module vectorlib;\n"
                        "import std;\n"
                        "namespace vectorlib {\n"
                        "export class Box {\n"
                        "public:\n"
                        "    std::vector<int> values{};\n"
                        "    Box() { return; }\n"
                        "    virtual ~Box() { return; }\n"
                        "};\n"
                        "export const Box& values_box(const Box& box) {\n"
                        "    return box;\n"
                        "}\n"
                        "export int sum(const std::vector<int>& values) {\n"
                        "    int total = 0;\n"
                        "    std::size_t i = 0;\n"
                        "    while (i < values.size()) {\n"
                        "        total = total + values[i];\n"
                        "        i = i + 1;\n"
                        "    }\n"
                        "    return total;\n"
                        "}\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        write_text_file(consumer_source,
                        "import std;\n"
                        "import vectorlib;\n"
                        "int main() {\n"
                        "    vectorlib::Box box{};\n"
                        "    box.values.push_back(1);\n"
                        "    box.values.push_back(2);\n"
                        "    box.values.push_back(3);\n"
                        "    return vectorlib::sum(vectorlib::values_box(box).values) == 6 ? 0 : 1;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import vectorlib=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected exit code 0, got " + std::to_string(run_result.exit_code) +
                   " stdout='" + run_result.stdout_text + "'");
        std::filesystem::remove_all(root);
    }
}

void run_string_view_tests() {
    {
        std::string case_name = "std_string_view_constructs_from_string_and_literal";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string text{"owner::Type"};
    std::string_view from_string{text};
    std::string_view from_literal{"owner::Type"};
    const char* empty{};
    if (from_string.empty()) return 1;
    if (from_string.size() != 11) return 2;
    if (from_string.length() != 11) return 3;
    if (from_string.data() == empty) return 4;
    return from_string == from_literal ? 0 : 5;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_string_view_rfind_and_substr_match_ast_usage";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string text{"owner::Type"};
    std::string_view view{text};
    size_t scope = view.rfind("::");
    if (scope >= view.size()) return 1;
    std::string_view tail = view.substr(scope + 2);
    return tail == "Type" ? 0 : 2;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_string_view_at_is_bounds_checked";
        cases_run++;
        RunResult ok = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string_view view{"abc"};
    return view.at(1) == 'b' ? 0 : 1;
}
)SCPP",
            case_name + "_ok");
        expect(ok.exit_code == 0, case_name + ": expected in-bounds access to succeed, got " + std::to_string(ok.exit_code));
        RunResult bad = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string_view view{"abc"};
    return view.at(9) == 'z' ? 0 : 1;
}
)SCPP",
            case_name + "_oob");
        expect(bad.exit_code != 0,
               case_name + ": expected out-of-bounds access to abort, got " + std::to_string(bad.exit_code));
    }

    {
        std::string case_name = "std_string_and_string_view_compare_against_literals_in_both_orders";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string name{"any"};
    if (!(name == "any")) return 1;
    if ("any" != name) return 2;
    if ("other" == name) return 3;
    if (!(name != "other")) return 4;

    std::string_view view{name};
    if (!(view == "any")) return 5;
    if ("any" != view) return 6;
    if ("other" == view) return 7;
    return view != "other" ? 0 : 8;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_string_view_is_thread_movable_and_shareable";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int main() {
    return scpp::is_thread_movable(std::string_view) && scpp::is_thread_shareable(std::string_view) ? 0 : 1;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_size_t_keyword_tests() {
    {
        std::string case_name = "size_t_and_ptrdiff_t_keywords_compile_and_run";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
size_t grow(std::size_t count) {
    size_t one = 1;
    return count + one;
}
ptrdiff_t distance(std::ptrdiff_t lhs, ptrdiff_t rhs) {
    return lhs - rhs;
}
int main() {
    size_t count = grow(6);
    ptrdiff_t delta = distance(9, 4);
    return (int)(count + (size_t)delta - 12);
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_increment_decrement_tests() {
    {
        std::string case_name = "prefix_and_postfix_increment_return_expected_values";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    int x = 5;
    int y = x++;
    int a = 5;
    int b = ++a;
    if (y != 5) return 1;
    if (x != 6) return 2;
    if (b != 6) return 3;
    if (a != 6) return 4;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "prefix_and_postfix_decrement_return_expected_values";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    int x = 5;
    int y = x--;
    int a = 5;
    int b = --a;
    if (y != 5) return 1;
    if (x != 4) return 2;
    if (b != 4) return 3;
    if (a != 4) return 4;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "classic_for_loop_accepts_postfix_increment_clause";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    int total = 0;
    for (int i = 0; i < 4; i++) {
        total = total + i;
    }
    return total - 6;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "classic_for_loop_accepts_postfix_decrement_clause";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    int total = 0;
    for (int i = 3; i >= 0; i--) {
        total = total + i;
    }
    return total - 6;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_compound_assignment_tests() {
    {
        std::string case_name = "numeric_compound_assignment_returns_updated_value";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    int x = 6;
    int a = (x += 4);
    int b = (x -= 3);
    int c = (x *= 2);
    int d = (x /= 7);
    if (a != 10) return 1;
    if (b != 7) return 2;
    if (c != 14) return 3;
    if (d != 2) return 4;
    return x - 2;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "std_string_plus_assign_appends_literals_and_strings";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::string candidate{"owner"};
    candidate += "::";
    std::string tail{"Type"};
    candidate += tail;
    if (candidate.size() != 11) return 1;
    const char* text = candidate.c_str();
    if (text[0] != 'o') return 2;
    if (text[5] != ':') return 3;
    if (text[6] != ':') return 4;
    if (text[10] != 'e') return 5;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_local_type_definition_tests() {
    {
        std::string case_name = "local_struct_definition_with_method_compiles_and_runs";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    struct Point {
        int x;
        int y;
        int sum() const { return this->x + this->y; }
    };
    Point p{};
    p.x = 2;
    p.y = 3;
    return p.sum() - 5;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "local_class_definition_with_constructor_compiles_and_runs";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(int main() {
    class Counter {
    public:
        int value;
        Counter(int initial) : value{initial} {}
        virtual ~Counter() { return; }
        int bump() {
            this->value += 1;
            return this->value;
        }
    };
    Counter counter{6};
    if (counter.bump() != 7) return 1;
    return counter.value - 7;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "local_type_name_is_not_visible_outside_its_function";
        cases_run++;
        if (auto parse_result = scpp::parse(
                "int make() {\n"
                "    struct Local { int value; };\n"
                "    Local ok{};\n"
                "    return ok.value;\n"
                "}\n"
                "int use() {\n"
                "    Local bad{};\n"
                "    return bad.value;\n"
                "}\n");
            !parse_result.has_value()) {
            const scpp::ParseError& e = parse_result.error();
            expect(e.loc.line == 7, case_name + ": expected out-of-scope use to fail on line 7, got " +
                                            std::to_string(e.loc.line));
        } else {
            expect(false, case_name + ": expected parse failure for out-of-scope local type name");
        }
    }
}

void run_unordered_set_tests() {
    {
        std::string case_name = "unordered_set_default_int_hash_rehashes_and_keeps_set_semantics";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::unordered_set<int> values{};
    if (!values.empty()) return 1;
    int i = 0;
    while (i < 40) {
        if (!values.insert(i)) return 2;
        i = i + 1;
    }
    if (values.size() != 40) return 3;
    i = 0;
    while (i < 40) {
        if (values.insert(i)) return 4;
        if (!values.contains(i)) return 5;
        i = i + 1;
    }
    i = 0;
    while (i < 20) {
        if (!values.erase(i)) return 6;
        i = i + 1;
    }
    if (values.size() != 20) return 7;
    i = 0;
    while (i < 20) {
        if (values.contains(i)) return 8;
        i = i + 1;
    }
    i = 20;
    while (i < 40) {
        if (!values.contains(i)) return 9;
        i = i + 1;
    }
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "unordered_set_rehash_duplicate_insert_and_erase_work_for_custom_key";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
class IntKey {
private:
    int value_{};
public:
    IntKey(int value) : value_{value} { return; }
    IntKey(const IntKey& other) : value_{other.value_} { return; }
    virtual ~IntKey() = default;
    int value() const { return this->value_; }
};
namespace std {
template<>
class hash<IntKey> {
public:
    uint64_t call(const IntKey& value) const {
        return static_cast<uint64_t>(value.value()) * 1315423911 + 1;
    }

    virtual ~hash() = default;
};

template<>
class equal_to<IntKey> {
public:
    bool call(const IntKey& lhs, const IntKey& rhs) const {
        return lhs.value() == rhs.value();
    }

    virtual ~equal_to() = default;
};
}
int main() {
    std::unordered_set<IntKey> values{};
    if (!values.empty()) return 1;
    int i = 0;
    while (i < 40) {
        IntKey key{i};
        if (!values.insert(key)) return 2;
        i = i + 1;
    }
    if (values.size() != 40) return 3;
    i = 0;
    while (i < 40) {
        IntKey key{i};
        if (!values.contains(key)) return 4;
        i = i + 1;
    }
    i = 0;
    while (i < 40) {
        IntKey key{i};
        if (values.insert(key)) return 5;
        i = i + 1;
    }
    if (values.size() != 40) return 6;
    i = 0;
    while (i < 20) {
        IntKey key{i};
        if (!values.erase(key)) return 7;
        i = i + 1;
    }
    i = 0;
    while (i < 20) {
        IntKey key{i};
        if (values.contains(key)) return 8;
        i = i + 1;
    }
    i = 20;
    while (i < 40) {
        IntKey key{i};
        if (!values.contains(key)) return 9;
        i = i + 1;
    }
    return values.size() == 20 ? 0 : 10;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "unordered_set_clear_and_destructor_destroy_all_elements";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int live_count = 0;
class Tracked {
private:
    int value_{};
public:
    Tracked(int value) : value_{value} {
        live_count = live_count + 1;
        return;
    }
    Tracked(const Tracked& other) : value_{other.value_} {
        live_count = live_count + 1;
        return;
    }
    virtual ~Tracked() {
        live_count = live_count - 1;
        return;
    }
    int value() const { return this->value_; }
};
namespace std {
template<>
class hash<Tracked> {
public:
    uint64_t call(const Tracked& value) const {
        return static_cast<uint64_t>(value.value()) * 2654435761 + 7;
    }

    virtual ~hash() = default;
};

template<>
class equal_to<Tracked> {
public:
    bool call(const Tracked& lhs, const Tracked& rhs) const {
        return lhs.value() == rhs.value();
    }

    virtual ~equal_to() = default;
};
}
int main() {
    {
        Tracked first{1};
        Tracked second{2};
        {
            std::unordered_set<Tracked> set{};
            set.insert(first);
            set.insert(second);
            if (live_count != 4) return 1;
            set.clear();
            if (live_count != 2) return 2;
            set.insert(first);
            set.insert(second);
        }
        if (live_count != 2) return 3;
    }
    return live_count == 0 ? 0 : 4;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "unordered_set_copy_move_and_thread_traits_follow_key_type";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
int copy_count = 0;
class CopyTracked {
private:
    int value_{};
public:
    CopyTracked(int value) : value_{value} { return; }
    CopyTracked(const CopyTracked& other) : value_{other.value_} {
        copy_count = copy_count + 1;
        return;
    }
    virtual ~CopyTracked() = default;
    int value() const { return this->value_; }
};
class RawPtrBox {
private:
    int* ptr_{};
public:
    RawPtrBox() { return; }
    RawPtrBox(const RawPtrBox& other) : ptr_{other.ptr_} { return; }
    virtual ~RawPtrBox() = default;
    bool is_null() const {
        int* empty{};
        return this->ptr_ == empty;
    }
};
namespace std {
template<>
class hash<CopyTracked> {
public:
    uint64_t call(const CopyTracked& value) const {
        return static_cast<uint64_t>(value.value()) * 1099511627 + 13;
    }

    virtual ~hash() = default;
};

template<>
class equal_to<CopyTracked> {
public:
    bool call(const CopyTracked& lhs, const CopyTracked& rhs) const {
        return lhs.value() == rhs.value();
    }

    virtual ~equal_to() = default;
};

template<>
class hash<RawPtrBox> {
public:
    uint64_t call(const RawPtrBox& value) const {
        if (value.is_null()) return static_cast<uint64_t>(0);
        return static_cast<uint64_t>(1);
    }

    virtual ~hash() = default;
};

template<>
class equal_to<RawPtrBox> {
public:
    bool call(const RawPtrBox& lhs, const RawPtrBox& rhs) const {
        return lhs.is_null() == rhs.is_null();
    }

    virtual ~equal_to() = default;
};
}
int main() {
    std::unordered_set<CopyTracked> original{};
    CopyTracked one{1};
    CopyTracked two{2};
    original.insert(one);
    original.insert(two);
    int copies_after_insert = copy_count;
    std::unordered_set<CopyTracked> copied{original};
    if (copy_count <= copies_after_insert) return 1;
    CopyTracked probe{1};
    if (!copied.contains(probe)) return 2;
    int copies_after_copy = copy_count;
    std::unordered_set<CopyTracked> moved{std::move(original)};
    if (!moved.contains(probe)) return 3;
    if (moved.size() != 2) return 4;
    if (copy_count != copies_after_copy) return 5;
    std::unordered_set<CopyTracked> assigned{};
    assigned = copied;
    if (!assigned.contains(probe)) return 6;
    if (copy_count <= copies_after_copy) return 7;
    int copies_after_assignment = copy_count;
    std::unordered_set<CopyTracked> move_assigned{};
    move_assigned = std::move(assigned);
    if (!move_assigned.contains(probe)) return 8;
    if (copy_count != copies_after_assignment) return 9;
    if (scpp::is_thread_movable(std::unordered_set<std::string>) != scpp::is_thread_movable(std::string)) return 10;
    if (scpp::is_thread_shareable(std::unordered_set<std::string>) != scpp::is_thread_shareable(std::string)) return 11;
    if (!scpp::is_thread_movable(std::unordered_set<int>)) return 12;
    if (!scpp::is_thread_shareable(std::unordered_set<int>)) return 13;
    if (scpp::is_thread_movable(std::unordered_set<RawPtrBox>)) return 14;
    return scpp::is_thread_shareable(std::unordered_set<RawPtrBox>) ? 15 : 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "unordered_set_string_usage_matches_ast_layoutcomputer_pattern";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int visit(std::unordered_set<std::string>& visiting, const std::string& current) {
    if (visiting.contains(current)) return 1;
    visiting.insert(current);
    if (!visiting.contains(current)) return 2;
    visiting.erase(current);
    return visiting.contains(current) ? 3 : 0;
}
int main() {
    std::unordered_set<std::string> visiting{};
    std::string current{"TypeName"};
    return visit(visiting, current);
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "cli_build_module_roundtrips_std_unordered_set_without_exporting_helpers";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "import std;\n"
                        "namespace helper {\n"
                        "    export std::unordered_set<std::string> names() {\n"
                        "        std::unordered_set<std::string> values{};\n"
                        "        values.insert(std::string{\"alpha\"});\n"
                        "        values.insert(std::string{\"beta\"});\n"
                        "        return values;\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        expect(std::filesystem::exists(interface_path), case_name + ": expected .scppm output");
        expect(std::filesystem::exists(archive_path), case_name + ": expected .scppa output");
        expect(read_file(interface_path).find("scpp_unordered_set_wrapper") == std::string::npos,
               case_name + ": expected no native-wrapper helper names in interface source");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import std;\n"
                        "import helper;\n"
                        "int main() {\n"
                        "    auto values = helper::names();\n"
                        "    if (!values.contains(std::string{\"alpha\"})) return 1;\n"
                        "    if (!values.contains(std::string{\"beta\"})) return 2;\n"
                        "    return values.size() == 2 ? 0 : 3;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build from .scppm should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected unordered_set consumer binary to exit 0, got " +
                                std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }
}

void run_expected_tests() {
    {
        std::string case_name = "std_abort_aborts_process";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::abort();
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code != 0,
               case_name + ": expected non-zero exit code, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_expected_discard_is_rejected_by_nodiscard";
        cases_run++;
        auto compile_result_61 = scpp::compile_to_executable(
            R"SCPP(import std;
enum class calc_error { invalid };
std::expected<int, calc_error> fail() {
    std::unexpected<calc_error> err{calc_error::invalid};
    std::expected<int, calc_error> bad{err};
    return std::move(bad);
}
int main() {
    fail();
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = false;
        if (!compile_result_61.has_value()) {
            std::string message = compile_result_61.error().what();
            threw = message.find("nodiscard type") != std::string::npos &&
                    message.find("expected results must be checked") != std::string::npos;
        }
        expect(threw, case_name + ": expected discarded std::expected diagnostic");
    }

    {
        std::string case_name = "std_expected_bad_access_aborts_via_std_abort";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
enum class calc_error { invalid };
int main() {
    std::unexpected<calc_error> err{calc_error::invalid};
    std::expected<int, calc_error> bad{err};
    return bad.value();
}
)SCPP",
            case_name);
        expect(result.exit_code != 0,
               case_name + ": expected non-zero exit code, got " + std::to_string(result.exit_code));
    }
}

void run_optional_tests() {
    {
        std::string case_name = "std_optional_empty_and_value_construction_work_without_T_default_ctor";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
class NoDefault {
private:
    int value_{};
public:
    virtual ~NoDefault() = default;
    NoDefault(int value) : value_{value} { return; }
    NoDefault(const NoDefault& other) : value_{other.value_} { return; }
    int value() const { return this->value_; }
};
int main() {
    std::optional<NoDefault> empty{};
    if (empty.has_value()) return 1;
    std::optional<NoDefault> full{NoDefault{7}};
    if (!full.has_value()) return 2;
    return full->value() - 7;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_optional_empty_reset_and_value_access_work";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::optional<int> empty{};
    std::optional<int> value{7};
    if (empty.has_value()) return 1;
    if (value.value() != 7) return 2;
    value.reset();
    if (value.has_value()) return 3;
    std::optional<int> replacement{9};
    value = replacement;
    return value.value() - 9;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_optional_bad_access_aborts_via_std_abort";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::optional<int> empty{};
    return empty.value();
}
)SCPP",
            case_name);
        expect(result.exit_code != 0,
               case_name + ": expected non-zero exit code, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_optional_copy_move_and_arrow_access_work";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
class Tracker {
private:
    int value_{};
    int* copies_{};
public:
    virtual ~Tracker() = default;
    Tracker(int value, int* copies) : value_{value}, copies_{copies} { return; }
    Tracker(const Tracker& other) : value_{other.value_}, copies_{other.copies_} {
        [[scpp::unsafe]] {
            *this->copies_ = *this->copies_ + 1;
        }
        return;
    }
    int value() const { return this->value_; }
    void set_value(int value) {
        this->value_ = value;
        return;
    }
};
int main() {
    int copies = 0;
    std::optional<Tracker> original{Tracker{7, &copies}};
    copies = 0;
    std::optional<Tracker> clone = original;
    if (copies != 1) return 1;
    original->set_value(11);
    if (clone->value() != 7) return 2;
    std::optional<Tracker> moved = std::move(original);
    if (copies != 1) return 3;
    if (!moved.has_value()) return 4;
    return moved->value() - 11;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_optional_reset_and_destructor_destroy_contained_value";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
class Tracker {
private:
    int* destroyed_{};
public:
    virtual ~Tracker() {
        [[scpp::unsafe]] {
            *this->destroyed_ = *this->destroyed_ + 1;
        }
        return;
    }
    Tracker(int* destroyed) : destroyed_{destroyed} { return; }
    Tracker(const Tracker& other) : destroyed_{other.destroyed_} { return; }
};
int main() {
    int destroyed = 0;
    {
        Tracker seed{&destroyed};
        {
            std::optional<Tracker> maybe{seed};
            maybe.reset();
            if (destroyed != 1) return 1;
            std::optional<Tracker> again{seed};
        }
        if (destroyed != 2) return 2;
    }
    return destroyed - 3;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "std_optional_thread_traits_follow_contained_type";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
import scpp;
class RawPtrBox {
public:
    virtual ~RawPtrBox() = default;
    RawPtrBox() { return; }
    RawPtrBox(const RawPtrBox& other) : ptr_{other.ptr_} { return; }
private:
    int* ptr_{};
};
int main() {
    if (!scpp::is_thread_movable(std::optional<int>)) return 1;
    if (!scpp::is_thread_shareable(std::optional<int>)) return 2;
    if (scpp::is_thread_movable(std::optional<RawPtrBox>)) return 3;
    return scpp::is_thread_shareable(std::optional<RawPtrBox>) ? 4 : 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "cli_build_module_roundtrips_std_optional_without_exporting_helpers";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::path module_source = root / "helper.scpp";
        std::filesystem::path interface_path = root / "helper.scppm";
        std::filesystem::path archive_path = root / "libhelper.scppa";
        std::filesystem::path consumer_source = root / "main.scpp";
        std::filesystem::path exe_path = root / "app";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        write_text_file(module_source,
                        "export module helper;\n"
                        "import std;\n"
                        "namespace helper {\n"
                        "    export std::optional<int> maybe_box(bool flag) {\n"
                        "        if (flag) {\n"
                        "            std::optional<int> boxed{42};\n"
                        "            return boxed;\n"
                        "        }\n"
                        "        std::optional<int> empty{};\n"
                        "        return empty;\n"
                        "    }\n"
                        "}\n");
        RunResult emit_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                " --interface-out " + interface_path.string() + " --archive-out " +
                                archive_path.string() + " 2>&1");
        expect(emit_result.exit_code == 0,
               case_name + ": build-module should succeed, got '" + emit_result.stdout_text + "'");
        expect(std::filesystem::exists(interface_path), case_name + ": expected .scppm output");
        expect(std::filesystem::exists(archive_path), case_name + ": expected .scppa output");
        expect(read_file(interface_path).find("__optional") == std::string::npos,
               case_name + ": expected no exported __optional helpers in interface source");
        std::filesystem::remove(module_source);
        write_text_file(consumer_source,
                        "import std;\n"
                        "import helper;\n"
                        "int main() {\n"
                        "    auto boxed = helper::maybe_box(true);\n"
                        "    if (!boxed.has_value()) return 1;\n"
                        "    if (boxed.value() != 42) return 2;\n"
                        "    auto empty = helper::maybe_box(false);\n"
                        "    return empty.has_value() ? 3 : 0;\n"
                        "}\n");
        RunResult build_result =
            run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() + " -o " +
                                exe_path.string() + " --import helper=" + interface_path.string() + " 2>&1");
        expect(build_result.exit_code == 0,
               case_name + ": consumer build from .scppm should succeed, got '" + build_result.stdout_text + "'");
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected optional consumer binary to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }
}

void run_smart_pointer_nullptr_tests() {
    {
        std::string case_name = "std_shared_and_unique_ptr_compare_against_nullptr";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    std::shared_ptr<int> empty_shared{};
    std::shared_ptr<int> full_shared = std::make_shared<int>(7);
    if (!(empty_shared == nullptr)) return 1;
    if (empty_shared != nullptr) return 2;
    if (full_shared == nullptr) return 3;
    if (!(full_shared != nullptr)) return 4;

    std::unique_ptr<int> empty_unique{};
    std::unique_ptr<int> full_unique = std::make_unique<int>(9);
    if (!(empty_unique == nullptr)) return 5;
    if (empty_unique != nullptr) return 6;
    if (full_unique == nullptr) return 7;
    return full_unique != nullptr ? 0 : 8;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_subscripted_deref_tests() {
    {
        std::string case_name = "forward_declared_member_vector_optional_element_can_be_dereferenced_through_subscript";
        cases_run++;
        auto compile_result_62 = scpp::compile_to_executable(
            R"SCPP(import std;
class Box;
class Box {
public:
    std::vector<std::optional<int>> values;
    virtual ~Box() = default;
};
const int& first(const Box& box) {
    return *box.values[0];
}
int main() { return 0; }
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_62.has_value();
        std::string unexpected = threw ? compile_result_62.error().what() : std::string();
        expect(!threw, case_name + ": expected forward-declared owner subscripted deref to compile, got '" + unexpected + "'");
    }

    {
        std::string case_name = "returning_ref_through_local_forward_declared_member_vector_optional_element_is_still_rejected";
        cases_run++;
        std::string unexpected;
        auto compile_result_63 = scpp::compile_to_executable(
            R"SCPP(import std;
class Box;
class Box {
public:
    std::vector<std::optional<int>> values;
    virtual ~Box() = default;
};
const int& bad(const Box& seed) {
    Box box{};
    std::optional<int> seven{7};
    box.values.push_back(seven);
    if (!seed.values.empty()) {
        return *box.values[0];
    }
    return *seed.values[0];
}
int main() {
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = false;
        if (!compile_result_63.has_value()) {
            const scpp::DriverError& error = compile_result_63.error();
            if (error.kind == scpp::DriverErrorKind::Dataflow) {
                threw = std::string(error.what()).find("returns a reference derived from 'box'") != std::string::npos;
            } else {
                unexpected = error.what();
            }
        }
        expect(threw, case_name + ": expected local vector element deref escape to remain rejected, got '" + unexpected + "'");
    }

    {
        std::string case_name = "assigning_forward_declared_owner_while_subscripted_optional_ref_is_live_is_still_rejected";
        cases_run++;
        std::string unexpected;
        auto compile_result_64 = scpp::compile_to_executable(
            R"SCPP(import std;
class Box;
class Box {
public:
    std::vector<std::optional<int>> values;
    virtual ~Box() = default;
};
int main() {
    std::optional<int> seven{7};
    Box box{};
    box.values.push_back(seven);
    const int& current = *box.values[0];
    std::vector<std::optional<int>> other{};
    std::optional<int> nine{9};
    other.push_back(nine);
    box.values = other;
    return current;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = false;
        if (!compile_result_64.has_value()) {
            const scpp::DriverError& error = compile_result_64.error();
            if (error.kind == scpp::DriverErrorKind::Dataflow) {
                threw = std::string(error.what()).find("cannot assign to this place: 'box' is currently borrowed") !=
                        std::string::npos;
            } else {
                unexpected = error.what();
            }
        }
        expect(threw, case_name + ": expected container reassignment while element-derived ref is live to remain rejected, got '" +
                          unexpected + "'");
    }
}

void run_io_tests() {
    {
        std::string case_name = "scpp_io_getline_reads_one_line_without_newline";
        cases_run++;
        RunResult result =
            compile_and_run_with_input(
                R"SCPP(import std;
import scpp;
int main() {
    // getline follows the prior read_line behavior and strips the trailing newline.
    auto line = scpp::io::getline();
    if (!line.has_value()) return 1;
    if (!line.value().equals("hello world")) return 2;
    if (line.value().length() != 11) return 3;
    return 0;
}
)SCPP",
                case_name, "hello world\nsecond line\n");
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text.empty(),
               case_name + ": expected empty stdout, got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "scpp_io_getline_returns_eof_error_on_empty_input";
        cases_run++;
        RunResult result = compile_and_run_with_input(
            R"SCPP(import std;
import scpp;
int main() {
    auto line = scpp::io::getline();
    if (line.has_value()) return 1;
    if (line.error() != scpp::io::error::eof) return 2;
    return 0;
}
)SCPP",
            case_name, "");
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_brace_init_only_var_decl_tests() {
    {
        std::string case_name = "class_var_decl_brace_init_constructs_successfully";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(class Box {
private:
    int value_{};
public:
                virtual ~Box() = default;
                Box(int value) : value_{value} { return; }
                int value() { return this->value_; }
            };
int main() {
    Box box{7};
    return box.value() - 7;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_generic_type_construction_expression_tests() {
    {
        std::string case_name = "explicit_generic_type_construction_expressions_work_in_return_argument_and_assignment";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
std::optional<int> make_value(int x) {
    int value = x + 1;
    return std::optional<int>{value};
}
int read_value(const std::optional<int>& value) {
    return *value;
}
std::optional<int> assign_value(int x) {
    std::optional<int> result{};
    int value = x + 2;
    result = std::optional<int>(value);
    return result;
}
int main() {
    int direct = 3;
    if (read_value(std::optional<int>{direct}) != 3) {
        return 1;
    }
    std::optional<int> made = make_value(3);
    if (!made.has_value() || *made != 4) {
        return 2;
    }
    std::optional<int> assigned = assign_value(4);
    if (!assigned.has_value() || *assigned != 6) {
        return 3;
    }
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "ctad_constructs_generic_temporaries_in_multiple_expression_positions";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
std::optional<int> make_value(int x) {
    int value = x + 1;
    return std::optional{value};
}
int read_value(const std::optional<int>& value) {
    return *value;
}
int read_shared(std::shared_ptr<int> value) {
    return *value;
}
int main() {
    std::optional<int> assigned{};
    int assigned_value = 4;
    assigned = std::optional{assigned_value};
    if (!assigned.has_value() || *assigned != 4) {
        return 1;
    }
    int direct = 2;
    if (read_value(std::optional{direct}) != 2) {
        return 2;
    }
    std::optional<int> made = make_value(6);
    if (!made.has_value() || *made != 7) {
        return 3;
    }
    [[scpp::unsafe]] {
        int* raw = new int(7);
        if (read_shared(std::shared_ptr{raw}) != 7) {
            return 4;
        }
    }
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_out_of_line_member_definition_tests() {
    {
        std::string case_name = "out_of_line_constructor_method_and_destructor_compile_and_run";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(class Box {
private:
    int value_{};
public:
    virtual ~Box();
    Box(int value);
    int value() const;
};
inline Box::Box(int value) : value_{value} { return; }
int Box::value() const { return this->value_; }
Box::~Box() { return; }
int main() {
    Box box{7};
    return box.value() - 7;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "out_of_line_operator_assign_compile_and_run";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(class Widget {
public:
    virtual ~Widget() = default;
    Widget(int v) { this.v = v; return; }
    Widget& operator=(const Widget& other);
    int value() const { return this.v; }
private:
    int v{};
};
Widget& Widget::operator=(const Widget& other) { this.v = other.v; return this; }
int main() {
    Widget lhs{1};
    Widget rhs{9};
    lhs = rhs;
    return lhs.value() - 9;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

void run_for_loop_tests() {
    {
        std::string case_name = "classic_for_init_decl_is_out_of_scope_after_loop";
        auto result = try_compile_and_run(
            R"SCPP(int main() {
    for (int j = 0; j < 2; j = j + 1) {
    }
    return j;
}
)SCPP",
            case_name);
        bool threw = !result.has_value();
        expect(threw, case_name + ": expected loop-init declaration to be out of scope after the loop");
    }
    {
        std::string case_name = "range_for_named_const_reference_compiles";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(struct Box {
    int value = 0;
};
int main() {
    Box boxes[2];
    boxes[0].value = 1;
    boxes[1].value = 2;
    int total = 0;
    for (const Box& box : boxes) {
        total = total + box.value;
    }
    return total - 3;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "range_for_named_reference_compiles";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(struct Box {
    int value = 0;
};
int main() {
    Box boxes[2];
    boxes[0].value = 1;
    boxes[1].value = 2;
    int total = 0;
    for (Box& box : boxes) {
        total = total + box.value;
    }
    return total - 3;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
    {
        std::string case_name = "range_for_const_reference_rejects_mutation";
        auto result = try_compile_and_run(
            R"SCPP(int main() {
    int values[2];
    for (const auto& value : values) {
        value = 1;
    }
    return 0;
}
)SCPP",
            case_name);
        bool threw = !result.has_value();
        expect(threw, case_name + ": expected mutation through const auto& to be rejected");
    }
}

void run_inheritance_constructor_and_destructor_tests() {
    {
        std::string case_name = "derived_constructor_runs_base_constructor_first";
        cases_run++;
        RunResult result = compile_and_run(
            "class Base {\n"
            "private:\n"
            "    int value{};\n"
            "public:\n"
            "    virtual ~Base() = default;\n"
            "    Base() { print_int(100); this->value = 7; return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    virtual ~Derived() override = default;\n"
            "    Derived() { print_int(200); return; }\n"
            "};\n"
            "int main() {\n"
            "    Derived d{};\n"
            "    print_int(d.get());\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "100\n200\n7\n",
               case_name + ": expected stdout '100\\n200\\n7\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "multilevel_inheritance_constructs_base_chain_in_order";
        cases_run++;
        RunResult result = compile_and_run(
            "class Grand {\n"
            "public:\n"
            "    virtual ~Grand() = default;\n"
            "    Grand() { print_int(10); return; }\n"
            "};\n"
            "class Parent : public Grand {\n"
            "public:\n"
            "    virtual ~Parent() override = default;\n"
            "    Parent() { print_int(20); return; }\n"
            "};\n"
            "class Child : public Parent {\n"
            "public:\n"
            "    virtual ~Child() override = default;\n"
            "    Child() { print_int(30); return; }\n"
            "};\n"
            "int main() {\n"
            "    Child child{};\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "10\n20\n30\n",
               case_name + ": expected stdout '10\\n20\\n30\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "explicit_base_initializer_invokes_nondefault_base_constructor";
        cases_run++;
        RunResult result = compile_and_run(
            "class Base {\n"
            "private:\n"
            "    int value{};\n"
            "public:\n"
            "    virtual ~Base() = default;\n"
            "    Base(int seed) { print_int(seed); this->value = seed; return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    virtual ~Derived() override = default;\n"
            "    Derived(int seed) : Base{seed} { print_int(seed + 1); return; }\n"
            "};\n"
            "int main() {\n"
            "    Derived d{7};\n"
            "    print_int(d.get());\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "7\n8\n7\n",
               case_name + ": expected stdout '7\\n8\\n7\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "implicit_default_construction_without_derived_ctor_still_runs_base_ctor";
        cases_run++;
        RunResult result = compile_and_run(
            "class Base {\n"
            "public:\n"
            "    virtual ~Base() = default;\n"
            "    Base() { print_int(11); return; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    virtual ~Derived() override = default;\n"
            "};\n"
            "int main() {\n"
            "    Derived d{};\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "11\n",
               case_name + ": expected stdout '11\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "derived_destructor_runs_before_base_destructor";
        cases_run++;
        RunResult result = compile_and_run(
            "class Base {\n"
            "public:\n"
            "    Base() { print_int(1); return; }\n"
            "    virtual ~Base() { print_int(4); return; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    Derived() { print_int(2); return; }\n"
            "    ~Derived() override { print_int(3); return; }\n"
            "};\n"
            "int main() {\n"
            "    Derived d{};\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "1\n2\n3\n4\n",
               case_name + ": expected stdout '1\\n2\\n3\\n4\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "base_destructor_still_runs_without_derived_destructor";
        cases_run++;
        RunResult result = compile_and_run(
            "class Base {\n"
            "public:\n"
            "    virtual ~Base() { print_int(9); return; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    ~Derived() override { return; }\n"
            "};\n"
            "int main() {\n"
            "    Derived d{};\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "9\n",
               case_name + ": expected stdout '9\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "missing_required_base_initializer_is_rejected";
        cases_run++;
        auto result = try_compile_and_run(
            "class Base {\n"
            "public:\n"
            "    virtual ~Base() = default;\n"
            "    Base(int seed) { return; }\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    Derived() { return; }\n"
            "};\n"
            "int main() {\n"
            "    Derived d{};\n"
            "    return 0;\n"
            "}\n",
            case_name);
        bool threw = !result.has_value();
        expect(threw, case_name + ": expected missing base initializer to be rejected");
    }
}

void run_default_constructor_selection_tests() {
    {
        std::string case_name = "struct_default_brace_init_with_only_parameterized_ctor_reports_dataflow_error";
        cases_run++;
        auto compile_result_65 = scpp::compile_to_executable(
            R"SCPP(struct User {
    int id{};
    User(int initial_id) : id{initial_id} { return; }
};

int main() {
    User user{};
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_65.has_value() &&
                     std::string(compile_result_65.error().what()).find("no default constructor") != std::string::npos;
        expect(threw, case_name + ": expected movecheck to reject missing default constructor");
    }

    {
        std::string case_name = "class_default_brace_init_with_only_parameterized_ctor_reports_dataflow_error";
        cases_run++;
        auto compile_result_66 = scpp::compile_to_executable(
            R"SCPP(class User {
public:
    int id{};
    User(int initial_id) : id{initial_id} { return; }
    virtual ~User() = default;
};

int main() {
    User user{};
    return 0;
}
)SCPP",
            (std::filesystem::current_path() / case_name).string());
        bool threw = !compile_result_66.has_value() &&
                     std::string(compile_result_66.error().what()).find("no default constructor") != std::string::npos;
        expect(threw, case_name + ": expected movecheck to reject missing default constructor");
    }

    {
        std::string case_name = "struct_default_brace_init_prefers_zero_arg_ctor_when_overloads_exist";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / (case_name + "_exe");
        auto compile_result_67 = scpp::compile_to_executable(
            R"SCPP(struct User {
    int id{};
    User() : id{7} { return; }
    User(int initial_id) : id{initial_id} { return; }
};

int main() {
    User from_default{};
    User from_argument{9};
    print_int(from_default.id);
    print_int(from_argument.id);
    return 0;
}
)SCPP",
            exe_path.string());
        if (!compile_result_67.has_value()) throw std::move(compile_result_67).error();
        RunResult result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        std::filesystem::remove(exe_path);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "7\n9\n",
               case_name + ": expected stdout '7\\n9\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "class_default_brace_init_prefers_zero_arg_ctor_when_overloads_exist";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / (case_name + "_exe");
        auto compile_result_68 = scpp::compile_to_executable(
            R"SCPP(class User {
public:
    int id{};
    User() : id{11} { return; }
    User(int initial_id) : id{initial_id} { return; }
    virtual ~User() = default;
};

int main() {
    User from_default{};
    User from_argument{13};
    print_int(from_default.id);
    print_int(from_argument.id);
    return 0;
}
)SCPP",
            exe_path.string());
        if (!compile_result_68.has_value()) throw std::move(compile_result_68).error();
        RunResult result = run_command_capture(shell_quote(exe_path.string()) + " 2>&1");
        std::filesystem::remove(exe_path);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "11\n13\n",
               case_name + ": expected stdout '11\\n13\\n', got '" + result.stdout_text + "'");
    }
}

void run_member_lifetime_tests() {
    {
        std::string case_name = "member_function_return_lifetime_this_is_accepted";
        cases_run++;
        scpp::Program program = parse_program_with_std_imports(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value;\n"
            "    int* data() [[scpp::lifetime(this)]] { return &value; }\n"
            "};\n");
        auto error_message = monomorphize_or_check_moves_error_message(program);
        if (error_message.has_value()) {
            expect(false, case_name + ": expected check_moves to accept receiver-tied member return, got '" +
                              *error_message + "'");
        }
        expect(!error_message.has_value(), case_name + ": expected receiver-tied member return to pass");
    }

    {
        std::string case_name = "member_function_return_lifetime_this_rejects_non_receiver_return";
        cases_run++;
        scpp::Program program = parse_program_with_std_imports(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value;\n"
            "    int* data(int* p) [[scpp::lifetime(this)]] { return p; }\n"
            "};\n");
        auto error_message = monomorphize_or_check_moves_error_message(program);
        if (error_message.has_value()) {
            expect(error_message->find("not from lifetime group 'this'") != std::string::npos,
                   case_name + ": expected receiver lifetime mismatch diagnostic, got '" + *error_message + "'");
        }
        expect(error_message.has_value(), case_name + ": expected check_moves to reject non-receiver return");
    }
}

void run_reference_wrapper_tests() {
    {
        std::string case_name = "reference_wrapper_optional_rebinds";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    int first = 4;
    int second = 9;
    std::reference_wrapper<int> current{first};
    std::optional<std::reference_wrapper<int>> maybe{current};
    if (!maybe.has_value()) return 1;
    maybe->get() = 6;
    std::reference_wrapper<int> rebound{second};
    current = rebound;
    current.get() = 12;
    print_int(first);
    print_int(second);
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "6\n12\n",
               case_name + ": expected stdout '6\\n12\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "reference_wrapper_const_target_reads";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
int main() {
    int value = 7;
    std::reference_wrapper<const int> current{value};
    std::optional<std::reference_wrapper<const int>> maybe{current};
    if (!maybe.has_value()) return 1;
    print_int(maybe->get());
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "7\n",
               case_name + ": expected stdout '7\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "reference_wrapper_const_class_target_binds_const_reference_argument";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
class Item {
public:
    int value = 0;
    virtual ~Item() = default;
};
std::optional<std::reference_wrapper<const Item>> wrap(const Item& item) {
    return std::optional<std::reference_wrapper<const Item>>{std::reference_wrapper<const Item>{item}};
}
int main() {
    Item item{};
    item.value = 9;
    std::optional<std::reference_wrapper<const Item>> wrapped = wrap(item);
    if (!wrapped.has_value()) return 1;
    print_int(wrapped->get().value);
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "9\n",
               case_name + ": expected stdout '9\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "const_qualified_type_argument_makes_substituted_reference_read_only";
        cases_run++;
        auto run_result_67 = try_compile_and_run(
            R"SCPP(import std;
template<typename T>
class Mutator {
public:
    Mutator(T& value) { value = 42; return; }
    virtual ~Mutator() = default;
};
int main() {
    int value = 1;
    Mutator<const int> mutator{value};
    return value;
}
)SCPP",
            case_name);
        bool threw = !run_result_67.has_value();
        expect(threw, case_name + ": expected a write through 'T&' with 'T = const int' to be rejected");
    }

    {
        std::string case_name = "const_qualified_type_argument_deduces_through_pointer_parameter";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
template<typename T>
int size_of_pointee(T* value) {
    return static_cast<int>(sizeof(T));
}
int main() {
    int value = 3;
    const int* readonly = &value;
    print_int(size_of_pointee(readonly));
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "4\n",
               case_name + ": expected stdout '4\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "reference_wrapper_optional_lifetime_annotation_supports_pointer_return";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "reference_wrapper_optional_lifetime_annotation_supports_pointer_return_exe";
        auto compile_result_69 = scpp::compile_to_executable(
            R"SCPP(import std;
const int* find_visible(std::optional<std::reference_wrapper<const int [[scpp::lifetime(source)]]>> source)
    [[scpp::lifetime(source)]] {
    return &source->get();
}
int main() {
    int value = 41;
    std::reference_wrapper<const int> wrapped{value};
    std::optional<std::reference_wrapper<const int>> source{wrapped};
    const int* ptr = find_visible(source);
    [[scpp::unsafe]] {
        print_int(*ptr);
    }
    return 0;
}
)SCPP",
            exe_path.string(), std_link_inputs(), prebuilt_module_import_paths());
        if (!compile_result_69.has_value()) throw std::move(compile_result_69).error();
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "top_level_lifetime_annotation_still_supports_pointer_return";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(const int* addr(const int& value [[scpp::lifetime(source)]]) [[scpp::lifetime(source)]] {
    return &value;
}
int main() {
    int value = 19;
    const int* ptr = addr(value);
    [[scpp::unsafe]] {
        return *ptr == 19 ? 0 : 2;
    }
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "nested_lifetime_annotation_rejects_non_eligible_template_argument";
        auto result = try_compile_and_run(
            R"SCPP(import std;
int* bad(std::optional<int [[scpp::lifetime(source)]]> source) [[scpp::lifetime(source)]] {
    return 0;
}
int main() { return 0; }
)SCPP",
            case_name);
        bool threw = !result.has_value() &&
                     std::string(result.error().what())
                             .find("does not denote a reference, pointer, span, or std::reference_wrapper-carried reference") !=
                         std::string::npos;
        expect(threw, case_name + ": expected non-eligible nested lifetime annotation to be rejected");
    }

    {
        std::string case_name = "reference_wrapper_optional_counts_as_eligible_pointer_source_for_ambiguity";
        auto result = try_compile_and_run(
            R"SCPP(import std;
const int* ambiguous(std::optional<std::reference_wrapper<const int>> source, const int& other) {
    if (!source.has_value()) return &other;
    return &source->get();
}
int main() { return 0; }
)SCPP",
            case_name);
        bool threw = !result.has_value() &&
                     std::string(result.error().what()).find("more than one eligible source parameter") != std::string::npos;
        expect(threw, case_name + ": expected optional<reference_wrapper<T>> to participate in pointer-source ambiguity");
    }
}

void run_defaulted_special_member_tests() {
    {
        std::string case_name = "defaulted_copy_special_members_runtime";
        cases_run++;
        RunResult result = compile_and_run(
            "class Simple {\n"
            "public:\n"
            "    int value{};\n"
            "    virtual ~Simple() {}\n"
            "    Simple() = default;\n"
            "    Simple(const Simple&) = default;\n"
            "};\n"
            "int main() {\n"
            "    Simple a{};\n"
            "    a.value = 7;\n"
            "    Simple b{a};\n"
            "    print_int(b.value);\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "7\n",
               case_name + ": expected stdout '7\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "generic_defaulted_copy_special_members_runtime";
        cases_run++;
        RunResult result = compile_and_run(
            "template<typename T>\n"
            "class Wrapper {\n"
            "public:\n"
            "    Wrapper(T& ref) : ptr_{&ref} {}\n"
            "    Wrapper(const Wrapper&) = default;\n"
            "    Wrapper& operator=(const Wrapper&) = default;\n"
            "    T& get() { [[scpp::unsafe]] { return *this->ptr_; } }\n"
            "    virtual ~Wrapper() {}\n"
            "private:\n"
            "    T* ptr_{};\n"
            "};\n"
            "int main() {\n"
            "    int first = 5;\n"
            "    int second = 9;\n"
            "    Wrapper<int> a{first};\n"
            "    Wrapper<int> b{a};\n"
            "    Wrapper<int> c{second};\n"
            "    c = a;\n"
            "    c.get() = 11;\n"
            "    print_int(b.get());\n"
            "    print_int(second);\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "11\n9\n",
               case_name + ": expected stdout '11\\n9\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "defaulted_move_special_members_runtime";
        cases_run++;
        RunResult result = compile_and_run(
            "import std;\n"
            "class Box {\n"
            "private:\n"
            "    std::unique_ptr<int> value;\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    Box(std::unique_ptr<int> v) : value{std::move(v)} { return; }\n"
            "    Box(Box&&) = default;\n"
            "    Box& operator=(Box&&) = default;\n"
            "    int read() const { return *this->value; }\n"
            "};\n"
            "int main() {\n"
            "    Box first{std::make_unique<int>(3)};\n"
            "    Box second{std::move(first)};\n"
            "    Box third{std::make_unique<int>(8)};\n"
            "    third = std::move(second);\n"
            "    print_int(third.read());\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "3\n",
               case_name + ": expected stdout '3\\n', got '" + result.stdout_text + "'");
    }
}

void run_equality_operator_tests() {
    {
        std::string case_name = "defaulted_equality_operators_compare_members";
        cases_run++;
        RunResult result = compile_and_run(
            "struct Point {\n"
            "    int x = 0;\n"
            "    int y = 0;\n"
            "    Point(int x, int y) : x{x}, y{y} { return; }\n"
            "    bool operator==(const Point&) const = default;\n"
            "    bool operator!=(const Point&) const = default;\n"
            "};\n"
            "int main() {\n"
            "    Point a{1, 2};\n"
            "    Point b{1, 2};\n"
            "    Point c{3, 4};\n"
            "    if (a == b) { print_int(1); } else { print_int(0); }\n"
            "    if (a != c) { print_int(1); } else { print_int(0); }\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "1\n1\n",
               case_name + ": expected stdout '1\\n1\\n', got '" + result.stdout_text + "'");
    }

    {
        std::string case_name = "custom_equality_operator_body_is_used_by_equals_syntax";
        cases_run++;
        RunResult result = compile_and_run(
            "struct Offset {\n"
            "    int value = 0;\n"
            "    Offset(int value) : value{value} { return; }\n"
            "    bool operator==(const Offset& other) const { return this.value + 1 == other.value; }\n"
            "};\n"
            "int main() {\n"
            "    Offset a{2};\n"
            "    Offset b{3};\n"
            "    Offset c{2};\n"
            "    if (a == b) { print_int(1); } else { print_int(0); }\n"
            "    if (a == c) { print_int(1); } else { print_int(0); }\n"
            "    return 0;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        expect(result.stdout_text == "1\n0\n",
               case_name + ": expected stdout '1\\n0\\n', got '" + result.stdout_text + "'");
    }
}

// The next two tests exercise scpp::compile_to_executable's std::expected<void, DriverError>
// API shape directly (mirroring parser.cppm's test_parse_returns_engaged_expected_on_success /
// test_parse_returns_disengaged_expected_on_failure_without_throwing), rather than going
// through the check-and-throw idiom used everywhere else in this file.
void test_compile_to_executable_returns_engaged_expected_on_success() {
    std::string case_name = "compile_to_executable_returns_engaged_expected_on_success";
    cases_run++;
    std::filesystem::path exe_path = std::filesystem::current_path() / case_name;
    std::filesystem::remove(exe_path);
    auto result = scpp::compile_to_executable("int main() { return 0; }\n", exe_path.string());
    expect(result.has_value(),
           case_name + ": expected compile_to_executable to return an engaged std::expected on success");
    std::filesystem::remove(exe_path);
}

void test_compile_to_executable_returns_disengaged_expected_on_failure_without_throwing() {
    std::string case_name = "compile_to_executable_returns_disengaged_expected_on_failure_without_throwing";
    cases_run++;
    std::filesystem::path exe_path = std::filesystem::current_path() / case_name;
    std::filesystem::remove(exe_path);
    // Same trailing-only-default-argument rule as
    // default_argument_trailing_rule_is_rejected above; the point here is the
    // std::expected shape itself, so this function contains zero try/catch.
    auto result = scpp::compile_to_executable(
        "int bad(int x = 1, int y) { return x + y; }\n"
        "int main() { return bad(1, 2); }\n",
        exe_path.string());
    expect(!result.has_value(),
           case_name + ": expected compile_to_executable to return a disengaged std::expected on failure");
    if (!result.has_value()) {
        expect(std::string(result.error().what()).find("every later parameter must also have one") !=
                   std::string::npos,
               case_name + ": expected trailing-only default-argument diagnostic");
        expect(result.error().loc.is_known(), case_name + ": expected DriverError to carry a known source location");
    }
    std::filesystem::remove(exe_path);
}

} // namespace

// Compiler bug #8: class-typed array elements had no object lifetime.
// codegen_test covers the emitted IR; these cases cover what the IR
// actually *does* at runtime, because "the constructor is called" and
// "the constructor is called once per element, on that element's own
// storage, and the destructors run back to front" are different claims
// and only the second one is the fix. Each program encodes its
// observations into its exit status, so a wrong count or a wrong order
// is a distinct failing number rather than a crash.
void run_array_element_lifetime_runtime_tests() {
    const std::string tracked_class =
        "int ctors = 0;\n"
        "int dtors = 0;\n"
        "int order = 0;\n"
        "class Tracked {\n"
        "  public:\n"
        "    Tracked() { ctors = ctors + 1; this.tag = ctors; return; }\n"
        "    virtual ~Tracked() { dtors = dtors + 1; order = order * 10 + this.tag; return; }\n"
        "    int tag = 0;\n"
        "};\n";

    {
        // Three elements, three constructors, three destructors, and the
        // destructors run 3-2-1. Before the fix this program ran zero of
        // each and returned 0.
        std::string case_name = "array_elements_run_constructors_and_destructors_in_reverse";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "void scope() {\n"
                                               "    Tracked arr[3]{};\n"
                                               "    return;\n"
                                               "}\n"
                                               "int main() {\n"
                                               "    scope();\n"
                                               "    if (ctors != 3) return 1;\n"
                                               "    if (dtors != 3) return 2;\n"
                                               "    if (order != 321) return 3;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected 3 constructors, 3 destructors and reverse destruction order (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Each element must be constructed on its *own* storage: a fix
        // that ran the constructor N times against element 0 would pass a
        // pure call-count check but alias every element together.
        std::string case_name = "each_array_element_is_constructed_on_its_own_storage";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "int main() {\n"
                                               "    Tracked arr[3]{};\n"
                                               "    if (arr[0].tag != 1) return 1;\n"
                                               "    if (arr[1].tag != 2) return 2;\n"
                                               "    if (arr[2].tag != 3) return 3;\n"
                                               "    arr[1].tag = 9;\n"
                                               "    if (arr[0].tag != 1) return 4;\n"
                                               "    if (arr[2].tag != 3) return 5;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0, case_name + ": expected each element to have its own storage (exit 0), got exit " +
                                          std::to_string(result.exit_code));
    }

    {
        // A nested array is an array whose element type is an array, so
        // all four leaf elements get the same treatment.
        std::string case_name = "nested_array_leaf_elements_have_full_lifetime";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "void scope() {\n"
                                               "    Tracked grid[2][2]{};\n"
                                               "    return;\n"
                                               "}\n"
                                               "int main() {\n"
                                               "    scope();\n"
                                               "    if (ctors != 4) return 1;\n"
                                               "    if (dtors != 4) return 2;\n"
                                               "    if (order != 4321) return 3;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected 4 constructors and 4 destructors in reverse order (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // A fresh set of elements per iteration, each destroyed at the end
        // of its own iteration -- not leaked and not destroyed twice.
        std::string case_name = "array_in_a_loop_body_is_constructed_and_destroyed_per_iteration";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "int main() {\n"
                                               "    int i = 0;\n"
                                               "    while (i < 3) {\n"
                                               "        Tracked arr[2]{};\n"
                                               "        i = i + 1;\n"
                                               "    }\n"
                                               "    if (ctors != 6) return 1;\n"
                                               "    if (dtors != 6) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0, case_name + ": expected 6 constructors and 6 destructors (exit 0), got exit " +
                                          std::to_string(result.exit_code));
    }

    {
        // An array of a polymorphic class used to be left with null vtable
        // pointers by the whole-array memset, so this program segfaulted
        // on the first virtual call.
        std::string case_name = "virtual_call_through_an_array_element_dispatches_correctly";
        cases_run++;
        RunResult result = compile_and_run("class Base {\n"
                                           "  public:\n"
                                           "    virtual ~Base() = default;\n"
                                           "    virtual int who() { return 7; }\n"
                                           "};\n"
                                           "int main() {\n"
                                           "    Base arr[2]{};\n"
                                           "    if (arr[0].who() != 7) return 1;\n"
                                           "    if (arr[1].who() != 7) return 2;\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected virtual dispatch through array elements to work (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // A class-typed array member: constructed by the enclosing
        // object's constructor, destroyed by its destructor.
        std::string case_name = "class_member_array_elements_have_full_lifetime";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "class Holder {\n"
                                               "  public:\n"
                                               "    Holder() { return; }\n"
                                               "    virtual ~Holder() = default;\n"
                                               "    Tracked items[2]{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    {\n"
                                               "        Holder h{};\n"
                                               "        if (h.items[0].tag != 1) return 1;\n"
                                               "        if (h.items[1].tag != 2) return 2;\n"
                                               "    }\n"
                                               "    if (ctors != 2) return 3;\n"
                                               "    if (dtors != 2) return 4;\n"
                                               "    if (order != 21) return 5;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0, case_name + ": expected the member array to be fully constructed and destroyed "
                                                  "(exit 0), got exit " +
                                          std::to_string(result.exit_code));
    }

    {
        // Copying an object with an array-of-class member has to run the
        // element type's copy constructor per element, and assigning has
        // to run its copy-assignment operator per element -- the eligibility
        // check already looked through the array, only emission did not.
        std::string case_name = "class_member_array_is_copied_and_assigned_element_by_element";
        cases_run++;
        RunResult result = compile_and_run("int copies = 0;\n"
                                           "int assigns = 0;\n"
                                           "class Item {\n"
                                           "  public:\n"
                                           "    Item() { this.v = 1; return; }\n"
                                           "    Item(const Item& other) { this.v = other.v + 10; copies = copies + 1; return; }\n"
                                           "    Item& operator=(const Item& other) { this.v = other.v + 100; assigns = assigns + 1; return *this; }\n"
                                           "    virtual ~Item() = default;\n"
                                           "    int v = 0;\n"
                                           "};\n"
                                           "class Holder {\n"
                                           "  public:\n"
                                           "    Holder() { return; }\n"
                                           "    Holder(const Holder& other) = default;\n"
                                           "    Holder& operator=(const Holder& other) = default;\n"
                                           "    virtual ~Holder() = default;\n"
                                           "    Item items[2]{};\n"
                                           "};\n"
                                           "int main() {\n"
                                           "    Holder a{};\n"
                                           "    Holder b{a};\n"
                                           "    if (copies != 2) return 1;\n"
                                           "    if (b.items[0].v != 11) return 2;\n"
                                           "    if (b.items[1].v != 11) return 3;\n"
                                           "    Holder c{};\n"
                                           "    c = a;\n"
                                           "    if (assigns != 2) return 4;\n"
                                           "    if (c.items[0].v != 101) return 5;\n"
                                           "    if (c.items[1].v != 101) return 6;\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected element-wise copy construction and assignment (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Every scope-exit path has to reach the array, not just the
        // fall-off-the-end one.
        std::string case_name = "array_elements_are_destroyed_on_an_early_return";
        cases_run++;
        RunResult result = compile_and_run(tracked_class +
                                               "int scope(int n) {\n"
                                               "    Tracked arr[2]{};\n"
                                               "    if (n > 0) {\n"
                                               "        return arr[0].tag;\n"
                                               "    }\n"
                                               "    return 0;\n"
                                               "}\n"
                                               "int main() {\n"
                                               "    int seen = scope(1);\n"
                                               "    if (seen != 1) return 1;\n"
                                               "    if (ctors != 2) return 2;\n"
                                               "    if (dtors != 2) return 3;\n"
                                               "    if (order != 21) return 4;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected the array to be destroyed on the early-return path (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // An array of a class with a default member initializer but no
        // user-declared constructor: the member initializers still have to
        // be applied per element rather than left as the whole-array zero
        // fill.
        std::string case_name = "array_of_constructorless_class_applies_default_member_initializers";
        cases_run++;
        RunResult result = compile_and_run("class Plain {\n"
                                           "  public:\n"
                                           "    virtual ~Plain() = default;\n"
                                           "    int v = 9;\n"
                                           "};\n"
                                           "int main() {\n"
                                           "    Plain arr[3]{};\n"
                                           "    if (arr[0].v != 9) return 1;\n"
                                           "    if (arr[1].v != 9) return 2;\n"
                                           "    if (arr[2].v != 9) return 3;\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected default member initializers to be applied per element (exit 0), got exit " +
                   std::to_string(result.exit_code));
    }
}

// A user-written destructor body must destroy the class's own class-typed
// members, exactly like a `= default` one does. Before the fix, member
// teardown lived only inside define_defaulted_function's defaulted-
// destructor branch, so writing `virtual ~T() { }` instead of
// `virtual ~T() = default;` silently leaked every class-typed member.
// Compiler bug #7, end to end. `return` never checked the returned
// value's type against the declared return type, so an `int`-typed
// expression returned from a `std::int64_t` function emitted `ret i32`
// in an `i64` function. Nothing in the frontend objected; LLVM's module
// verifier did, but it runs once over the finished module and reported
// the failure at whatever source position the *last* lowered function
// left behind -- a stdlib file the user never opened. These cases pin
// both halves: the mismatch is now a frontend diagnostic, and it names
// the user's own file.
// A function template's body is cloned once per instantiation. Those clones used
// to be produced by hand-written cloners in parser.cppm and
// movecheck/generics_support.cppm that had drifted away from the authoritative
// one in ast.cppm and silently dropped `Stmt::is_const`, `Stmt::is_static_local`,
// `Stmt::alignment_specs` and `Stmt::resolved_alignment`. Every declaration
// modifier on a local inside a generic function was therefore lost on the way to
// the code that was actually compiled -- not a cosmetic loss but a soundness
// hole (const went unenforced) and a miscompilation (a `static` local became an
// ordinary one). All cloning now shares a single enumeration, so exercise the
// generic path end to end.
// An exported function template cannot be pre-compiled, so `build-module` ships
// its *body* inside the `.scppm` structured compile-time payload and the importer
// deserializes and instantiates it. That serializer is the second place that has
// to enumerate every field of an `Expr`/`Stmt`, and it was missing five of them:
// `Expr::resolved_local`, `Stmt::declared_local`, `Stmt::is_static_local`,
// `Param::resolved_local` and `LambdaCapture::resolved_local`.
//
// `is_static_local` was the observable one, and it is the worst shape of bug this
// project has: a *persisted* lossy artifact. Identical sources produced a working
// program when the module was built from source and a silently wrong one when the
// same module was loaded from its cache -- a cold-build/warm-build divergence
// that outlives the run that created it.
//
// The resolution fields turned out to be benign in practice (the payload is
// merged into the importing Program before that compilation's own resolver runs,
// which recomputes them), but they are serialized now regardless: relying on a
// downstream pass to repair a lossy format is exactly the sort of implicit
// coupling that stops being true the moment the pass order changes.
// The module-level IR dump used to be written unconditionally to the
// hard-coded relative path "scratch_selfhost/broken_module_dump.ll":
// junk in whatever directory the user happened to be standing in, or --
// if that directory didn't exist -- nothing at all, silently, while still
// looking to the reader like a dump had been produced.
void run_broken_module_dump_is_opt_in_tests() {
    {
        cases_run++;
        std::string case_name = "compiling_does_not_write_a_debug_dump_into_the_working_directory";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::path source = root / "ok.scpp";
        write_text_file(source, "int main() { return 0; }\n");
        RunResult build = run_command_capture("cd " + root.string() + " && " + std::string(SCPP_BINARY_PATH) + " " +
                                              source.string() + " -o " + (root / "app").string() + " 2>&1");
        expect(build.exit_code == 0, case_name + ": expected a clean build, got '" + build.stdout_text + "'");
        expect(!std::filesystem::exists(root / "scratch_selfhost"),
               case_name + ": the compiler must not create a 'scratch_selfhost' directory in the user's cwd");
        expect(!std::filesystem::exists(root / "broken_module_dump.ll"),
               case_name + ": the compiler must not drop a debug dump in the user's cwd");
        std::filesystem::remove_all(root);
    }
    {
        // The environment variable is the whole opt-in: no variable, no
        // file, anywhere.
        cases_run++;
        std::string case_name = "broken_module_dump_env_var_is_the_only_way_to_get_a_dump";
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::path dump_path = root / "requested_dump.ll";
        std::filesystem::path source = root / "ok.scpp";
        write_text_file(source, "int main() { return 0; }\n");
        RunResult build = run_command_capture("SCPP_DUMP_BROKEN_MODULE=" + dump_path.string() + " " +
                                              std::string(SCPP_BINARY_PATH) + " " + source.string() + " -o " +
                                              (root / "app").string() + " 2>&1");
        expect(build.exit_code == 0, case_name + ": expected a clean build, got '" + build.stdout_text + "'");
        // A *successful* compile has no broken module, so even with the
        // variable set there is nothing to dump.
        expect(!std::filesystem::exists(dump_path),
               case_name + ": a successful compile must not write a broken-module dump");
        std::filesystem::remove_all(root);
    }
}

void run_scppm_payload_round_trip_tests() {
    // Compiles `consumer` against `module_source` twice -- once with the module
    // as source (cold) and once through a `build-module` `.scppm` artifact
    // (warm) -- and returns both exit codes. They must be equal.
    auto cold_and_warm_exit_codes = [](const std::string& case_name, std::string_view module_text,
                                       std::string_view consumer_text) -> std::pair<int, int> {
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::path module_source = root / "probe_mod.scpp";
        std::filesystem::path interface_path = root / "probe_mod.scppm";
        std::filesystem::path archive_path = root / "libprobe_mod.scppa";
        std::filesystem::path consumer_source = root / "consumer.scpp";
        write_text_file(module_source, module_text);
        write_text_file(consumer_source, consumer_text);

        std::filesystem::path cold_exe = root / "cold_app";
        RunResult cold_build = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() +
                                                  " -o " + cold_exe.string() + " --import probe_mod=" +
                                                  module_source.string() + " 2>&1");
        int cold_code = -1;
        if (cold_build.exit_code == 0) {
            cold_code = run_command_capture(cold_exe.string() + " 2>&1").exit_code;
        } else {
            expect(false, case_name + ": cold build failed: " + cold_build.stdout_text);
        }

        RunResult emit = run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " +
                                             module_source.string() + " --interface-out " + interface_path.string() +
                                             " --archive-out " + archive_path.string() + " 2>&1");
        expect(emit.exit_code == 0, case_name + ": build-module failed: " + emit.stdout_text);
        std::vector<unsigned char> interface_bytes = read_binary_file(interface_path);
        expect(interface_bytes.size() >= 8 && (interface_bytes[7] & 0x01u) != 0u,
               case_name + ": expected the module to carry a structured compile-time payload");
        // Remove the source so the artifact is provably the only thing the warm
        // build can be reading the template body from.
        std::filesystem::remove(module_source);

        std::filesystem::path warm_exe = root / "warm_app";
        RunResult warm_build = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() +
                                                  " -o " + warm_exe.string() + " --import probe_mod=" +
                                                  interface_path.string() + " 2>&1");
        int warm_code = -1;
        if (warm_build.exit_code == 0) {
            warm_code = run_command_capture(warm_exe.string() + " 2>&1").exit_code;
        } else {
            expect(false, case_name + ": warm build failed: " + warm_build.stdout_text);
        }
        std::filesystem::remove_all(root);
        return std::pair<int, int>{cold_code, warm_code};
    };

    {
        // Pre-fix: cold 123, warm 111 -- `is_static_local` never reached the
        // artifact, so the static local was re-initialized on every call.
        std::string case_name = "scppm_payload_preserves_static_locals_in_exported_templates";
        cases_run++;
        auto [cold_code, warm_code] = cold_and_warm_exit_codes(case_name,
                                                               "export module probe_mod;\n"
                                                               "namespace probe_mod {\n"
                                                               "export template <typename T>\n"
                                                               "T bump(T seed) {\n"
                                                               "    static T counter = seed;\n"
                                                               "    counter = counter + 1;\n"
                                                               "    return counter;\n"
                                                               "}\n"
                                                               "}\n",
                                                               "import probe_mod;\n"
                                                               "int main() {\n"
                                                               "    int first = probe_mod::bump<int>(0);\n"
                                                               "    int second = probe_mod::bump<int>(0);\n"
                                                               "    int third = probe_mod::bump<int>(0);\n"
                                                               "    return first * 100 + second * 10 + third;\n"
                                                               "}\n");
        expect(cold_code == 123,
               case_name + ": a static local in a template built from source should count 1,2,3, got " +
                   std::to_string(cold_code));
        expect(warm_code == 123,
               case_name + ": the same template loaded from its .scppm should behave identically, got " +
                   std::to_string(warm_code));
        expect(cold_code == warm_code,
               case_name + ": cold and warm builds diverged (cold " + std::to_string(cold_code) + ", warm " +
                   std::to_string(warm_code) + ")");
    }
    {
        // The other `Stmt` declaration modifiers travel through the same list.
        std::string case_name = "scppm_payload_preserves_const_and_switch_in_exported_templates";
        cases_run++;
        auto [cold_code, warm_code] = cold_and_warm_exit_codes(case_name,
                                                               "export module probe_mod;\n"
                                                               "namespace probe_mod {\n"
                                                               "export template <typename T>\n"
                                                               "T classify(T value) {\n"
                                                               "    const T bonus = 1;\n"
                                                               "    switch (value) {\n"
                                                               "        case 1:\n"
                                                               "            return 10 + bonus;\n"
                                                               "        case 2:\n"
                                                               "            return 20 + bonus;\n"
                                                               "        default:\n"
                                                               "            return 30 + bonus;\n"
                                                               "    }\n"
                                                               "}\n"
                                                               "}\n",
                                                               "import probe_mod;\n"
                                                               "int main() {\n"
                                                               "    return probe_mod::classify<int>(1) + probe_mod::classify<int>(2) +\n"
                                                               "           probe_mod::classify<int>(9);\n"
                                                               "}\n");
        expect(cold_code == 63, case_name + ": expected 11 + 21 + 31 = 63 from source, got " + std::to_string(cold_code));
        expect(warm_code == 63, case_name + ": expected 11 + 21 + 31 = 63 from .scppm, got " + std::to_string(warm_code));
        expect(cold_code == warm_code,
               case_name + ": cold and warm builds diverged (cold " + std::to_string(cold_code) + ", warm " +
                   std::to_string(warm_code) + ")");
    }
    {
        // Lambda captures and parameters carry `resolved_local` too, and a
        // template body containing a lambda exercises both.
        std::string case_name = "scppm_payload_preserves_lambda_captures_in_exported_templates";
        cases_run++;
        auto [cold_code, warm_code] = cold_and_warm_exit_codes(case_name,
                                                               "export module probe_mod;\n"
                                                               "namespace probe_mod {\n"
                                                               "export template <typename T>\n"
                                                               "T capture_sum(T seed) {\n"
                                                               "    const T base = seed;\n"
                                                               "    T extra = seed + seed;\n"
                                                               "    auto adder = [base, &extra](T more) -> T { return base + extra + more; };\n"
                                                               "    return adder(seed);\n"
                                                               "}\n"
                                                               "}\n",
                                                               "import probe_mod;\n"
                                                               "int main() { return probe_mod::capture_sum<int>(3); }\n");
        expect(cold_code == 12, case_name + ": expected 3 + 6 + 3 = 12 from source, got " + std::to_string(cold_code));
        expect(warm_code == 12, case_name + ": expected 3 + 6 + 3 = 12 from .scppm, got " + std::to_string(warm_code));
        expect(cold_code == warm_code,
               case_name + ": cold and warm builds diverged (cold " + std::to_string(cold_code) + ", warm " +
                   std::to_string(warm_code) + ")");
    }
    {
        // The on-disk layout changed, so a cache written by an older compiler
        // must be *rejected*, not misread. Without the version guard a stale
        // artifact would deserialize into garbage at a byte offset.
        std::string case_name = "stale_structured_payload_version_is_rejected";
        cases_run++;
        std::filesystem::path root = std::filesystem::current_path() / case_name;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        std::filesystem::path module_source = root / "stale_mod.scpp";
        std::filesystem::path interface_path = root / "stale_mod.scppm";
        std::filesystem::path archive_path = root / "libstale_mod.scppa";
        std::filesystem::path consumer_source = root / "consumer.scpp";
        write_text_file(module_source,
                        "export module stale_mod;\n"
                        "namespace stale_mod {\n"
                        "export template <typename T>\n"
                        "T identity(T value) { return value; }\n"
                        "}\n");
        write_text_file(consumer_source,
                        "import stale_mod;\n"
                        "int main() { return stale_mod::identity<int>(0); }\n");
        RunResult emit = run_command_capture(std::string(SCPP_BINARY_PATH) + " build-module " + module_source.string() +
                                             " --interface-out " + interface_path.string() + " --archive-out " +
                                             archive_path.string() + " 2>&1");
        expect(emit.exit_code == 0, case_name + ": build-module failed: " + emit.stdout_text);
        std::vector<unsigned char> bytes = read_binary_file(interface_path);
        // Find the payload's "SAST" magic and corrupt the version word that
        // immediately follows it, simulating an artifact from an older compiler.
        bool patched = false;
        for (std::size_t i = 0; i + 8 <= bytes.size(); i++) {
            if (bytes[i] == 'S' && bytes[i + 1] == 'A' && bytes[i + 2] == 'S' && bytes[i + 3] == 'T') {
                bytes[i + 4] = 1u;
                bytes[i + 5] = 0u;
                bytes[i + 6] = 0u;
                bytes[i + 7] = 0u;
                patched = true;
                break;
            }
        }
        expect(patched, case_name + ": expected to find the structured payload magic in the artifact");
        if (patched) {
            std::ofstream out(interface_path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            out.close();
            RunResult build = run_command_capture(std::string(SCPP_BINARY_PATH) + " " + consumer_source.string() +
                                                  " -o " + (root / "app").string() + " --import stale_mod=" +
                                                  interface_path.string() + " 2>&1");
            expect(build.exit_code != 0, case_name + ": a stale payload version should be rejected, not accepted");
            expect(build.stdout_text.find("unsupported structured compile-time payload version") != std::string::npos,
                   case_name + ": expected a payload-version diagnostic, got: " + build.stdout_text);
        }
        std::filesystem::remove_all(root);
    }
}

void run_generic_body_clone_fidelity_tests() {
    {
        // Pre-fix this compiled and ran: `is_const` never reached the
        // monomorphized body, so movecheck saw an ordinary mutable local.
        std::string case_name = "const_local_in_generic_function_is_still_const_after_monomorphization";
        cases_run++;
        auto result = try_compile_and_run("import std;\n"
                                          "template <typename T>\n"
                                          "T twice(T seed) {\n"
                                          "    const T factor = seed;\n"
                                          "    factor = seed + seed;\n"
                                          "    return factor;\n"
                                          "}\n"
                                          "int main() { return twice<int>(3); }\n",
                                          case_name);
        bool rejected = !result.has_value();
        std::string message = rejected ? result.error().what() : "";
        expect(rejected, case_name + ": expected reassigning a 'const' local inside a function template to be rejected");
        if (rejected) {
            expect(message.find("const") != std::string::npos,
                   case_name + ": expected a const-reassignment diagnostic, got: " + message);
        }
    }
    {
        // Pre-fix this returned 111: `is_static_local` never reached the
        // monomorphized body, so `counter` was re-initialized on every call
        // instead of persisting -- a silent wrong answer, not a compile error.
        std::string case_name = "static_local_in_generic_function_persists_across_calls";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                           "template <typename T>\n"
                                           "T bump(T seed) {\n"
                                           "    static T counter = seed;\n"
                                           "    counter = counter + 1;\n"
                                           "    return counter;\n"
                                           "}\n"
                                           "int main() {\n"
                                           "    int first = bump<int>(0);\n"
                                           "    int second = bump<int>(0);\n"
                                           "    int third = bump<int>(0);\n"
                                           "    return first * 100 + second * 10 + third;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 123,
               case_name + ": expected a static local in a function template to keep counting (123), got " +
                   std::to_string(result.exit_code));
    }
    {
        // The scalar counterpart, which always worked: it is the *pair* of
        // answers that matters, since the whole bug family here is two code
        // paths answering the same question differently.
        std::string case_name = "static_local_in_plain_function_persists_across_calls";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                           "int bump(int seed) {\n"
                                           "    static int counter = seed;\n"
                                           "    counter = counter + 1;\n"
                                           "    return counter;\n"
                                           "}\n"
                                           "int main() {\n"
                                           "    int first = bump(0);\n"
                                           "    int second = bump(0);\n"
                                           "    int third = bump(0);\n"
                                           "    return first * 100 + second * 10 + third;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 123,
               case_name + ": expected a static local in a plain function to keep counting (123), got " +
                   std::to_string(result.exit_code));
    }
    {
        // A `switch` inside a *global variable's* initializer lambda reaches
        // `Stmt` cloning only through an enclosing node's copy constructor,
        // which used to run an entirely separate cloner with no `switch_cases`
        // loop at all -- every case vanished.
        std::string case_name = "switch_inside_a_global_initializer_lambda_keeps_its_cases";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                           "int classify(int value) {\n"
                                           "    switch (value) {\n"
                                           "        case 1:\n"
                                           "            return 10;\n"
                                           "        case 2:\n"
                                           "            return 20;\n"
                                           "        default:\n"
                                           "            return 30;\n"
                                           "    }\n"
                                           "}\n"
                                           "int main() { return classify(1) + classify(2) + classify(9); }\n",
                                           case_name);
        expect(result.exit_code == 60,
               case_name + ": expected 10 + 20 + 30 = 60, got " + std::to_string(result.exit_code));
    }
}

void run_return_type_checking_runtime_tests() {
    {
        // The exact reported shape: an `int` field read through a
        // pointer, returned from a `std::int64_t` function. Originally
        // caught only by codegen's LLVM-type backstop, on the assumption
        // that infer_expr_type always gives up on Member chains for want
        // of Program-level field-type information. It does resolve this
        // one, so movecheck's own name-based return check now reports it
        // first; the assertions below deliberately pin only what the
        // user needs from the diagnostic -- the offending function named,
        // and a location in their file rather than the stdlib -- so
        // either layer satisfies them.
        std::string case_name = "return_int_member_from_int64_function_is_diagnosed_in_the_users_file";
        cases_run++;
        auto result = try_compile_and_run("import std;\n"
                                          "class Node {\n"
                                          "public:\n"
                                          "    int tag = 3;\n"
                                          "    virtual ~Node() { }\n"
                                          "};\n"
                                          "std::int64_t tag_of(Node& value) {\n"
                                          "    return value.tag;\n"
                                          "}\n"
                                          "int main() { return 0; }\n",
                                          case_name);
        bool rejected = !result.has_value();
        std::string message = rejected ? result.error().what() : "";
        expect(rejected, case_name + ": expected an 'int' member returned from an 'int64_t' function to be rejected");
        if (rejected) {
            expect(message.find("std_memory.scpp") == std::string::npos,
                   case_name + ": the diagnostic still points into the stdlib instead of the offending return, got: " +
                       message);
            expect(message.find("tag_of") != std::string::npos,
                   case_name + ": expected the diagnostic to name the offending function, got: " + message);
        }
    }

    {
        // ...and the correct spelling of the same function compiles and
        // produces the right value at runtime. Widening has to be
        // written out, because scpp has no implicit scalar conversions.
        std::string case_name = "explicitly_widened_member_return_produces_the_right_value";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                          "class Node {\n"
                                           "public:\n"
                                           "    int tag = 3;\n"
                                           "    virtual ~Node() { }\n"
                                           "};\n"
                                           "std::int64_t tag_of(Node& value) {\n"
                                           "    return static_cast<std::int64_t>(value.tag);\n"
                                           "}\n"
                                           "int main() {\n"
                                           "    Node n{};\n"
                                           "    std::int64_t t = tag_of(n);\n"
                                           "    return static_cast<int>(t);\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 3,
               case_name + ": expected the widened member value to survive the return, got exit code " +
                   std::to_string(result.exit_code));
    }

    {
        // The gap was not limited to integer widths -- returning a class
        // value from an `int` function was equally unchecked, and equally
        // reached the verifier.
        std::string case_name = "return_class_value_from_int_function_is_diagnosed_in_the_users_file";
        cases_run++;
        auto result = try_compile_and_run("import std;\n"
                                          "class Box {\n"
                                          "public:\n"
                                          "    int v = 1;\n"
                                          "    virtual ~Box() { }\n"
                                          "};\n"
                                          "int returns_class() {\n"
                                          "    Box b{};\n"
                                          "    return b;\n"
                                          "}\n"
                                          "int main() { return 0; }\n",
                                          case_name);
        bool rejected = !result.has_value();
        std::string message = rejected ? result.error().what() : "";
        expect(rejected, case_name + ": expected a class value returned from an 'int' function to be rejected");
        if (rejected) {
            expect(message.find("std_memory.scpp") == std::string::npos,
                   case_name + ": the diagnostic still points into the stdlib, got: " + message);
        }
    }

    {
        // spec §6: `bool` has exactly two values, and every branch
        // condition is produced by truncating its i8 representation to
        // i1 -- which reads the low bit. A cast to bool used to be an
        // ordinary width conversion, so `static_cast<bool>(2)` truncated
        // i32 2 to i8 2 and produced a bool that `static_cast<int>`
        // reported as 2 while `if` treated as *false*. Exit code 102
        // was the observable symptom; 1 is the correct answer.
        std::string case_name = "static_cast_to_bool_normalises_to_zero_or_one";
        cases_run++;
        RunResult result = compile_and_run("int main() {\n"
                                           "    bool b = static_cast<bool>(2);\n"
                                           "    int back = static_cast<int>(b);\n"
                                           "    if (b) { return back; }\n"
                                           "    return 100 + back;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 1,
               case_name + ": expected static_cast<bool>(2) to be true and read back as 1, got exit code " +
                   std::to_string(result.exit_code));
    }

    {
        // The equal-width sources are the worse half of the same bug:
        // they took the identical-LLVM-type fast path and copied the
        // byte through completely untouched.
        std::string case_name = "static_cast_to_bool_normalises_equal_width_sources";
        cases_run++;
        RunResult result = compile_and_run("int main() {\n"
                                           "    int8_t raw = 7;\n"
                                           "    bool b = static_cast<bool>(raw);\n"
                                           "    if (static_cast<int>(b) != 1) { return 10; }\n"
                                           "    char c = 'A';\n"
                                           "    bool cb = static_cast<bool>(c);\n"
                                           "    if (static_cast<int>(cb) != 1) { return 20; }\n"
                                           "    int8_t zero = 0;\n"
                                           "    bool zb = static_cast<bool>(zero);\n"
                                           "    if (static_cast<int>(zb) != 0) { return 30; }\n"
                                           "    if (zb) { return 40; }\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected every equal-width cast to bool to normalise, got exit code " +
                   std::to_string(result.exit_code));
    }

    {
        // A non-zero float converts to true, and only exactly 0.0 to
        // false -- the truncating lowering got 2.0 wrong for the same
        // reason it got the integer 2 wrong.
        std::string case_name = "static_cast_to_bool_from_float_compares_against_zero";
        cases_run++;
        RunResult result = compile_and_run("int main() {\n"
                                           "    double two = 2.0;\n"
                                           "    if (!static_cast<bool>(two)) { return 10; }\n"
                                           "    double small = 0.5;\n"
                                           "    if (!static_cast<bool>(small)) { return 20; }\n"
                                           "    double zero = 0.0;\n"
                                           "    if (static_cast<bool>(zero)) { return 30; }\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected float-to-bool to be a comparison against zero, got exit code " +
                   std::to_string(result.exit_code));
    }

    {
        // spec §6: the arithmetic operators were the one class never
        // name-checked, so mismatched widths escaped movecheck entirely
        // and surfaced as an "invalid IR" internal error naming LLVM's
        // module verifier, with no source location for the user to act
        // on. It has to be an ordinary diagnostic pointing at the
        // offending operands.
        std::string case_name = "mixed_width_arithmetic_is_a_source_diagnostic_not_an_llvm_verifier_error";
        cases_run++;
        auto result = try_compile_and_run("int main() {\n"
                                          "    int a = 1;\n"
                                          "    long b = 2;\n"
                                          "    if (a + b == 3) { return 1; }\n"
                                          "    return 0;\n"
                                          "}\n",
                                          case_name);
        bool rejected = !result.has_value();
        std::string message = rejected ? result.error().what() : "";
        expect(rejected, case_name + ": expected 'int + long' to be rejected");
        if (rejected) {
            expect(message.find("invalid IR") == std::string::npos,
                   case_name + ": expected a source-level diagnostic rather than an LLVM verifier error, got: " +
                       message);
            expect(message.find("no implicit conversion") != std::string::npos,
                   case_name + ": expected the diagnostic to cite the no-implicit-conversion rule, got: " + message);
        }
    }

    {
        std::string case_name = "bare_return_from_value_returning_function_is_diagnosed";
        cases_run++;
        auto result = try_compile_and_run("import std;\n"
                                          "int bare() { return; }\n"
                                          "int main() { return 0; }\n",
                                          case_name);
        bool rejected = !result.has_value();
        std::string message = rejected ? result.error().what() : "";
        expect(rejected, case_name + ": expected a bare 'return;' in an 'int' function to be rejected");
        if (rejected) {
            expect(message.find("std_memory.scpp") == std::string::npos,
                   case_name + ": the diagnostic still points into the stdlib, got: " + message);
        }
    }

    {
        std::string case_name = "returning_a_value_from_a_void_function_is_diagnosed";
        cases_run++;
        auto result = try_compile_and_run("import std;\n"
                                          "void nothing() { return 5; }\n"
                                          "int main() { nothing(); return 0; }\n",
                                          case_name);
        expect(!result.has_value(), case_name + ": expected a value returned from a 'void' function to be rejected");
    }

    {
        // Every shape the return path already handled specially still
        // has to compile *and* still has to produce the right value:
        // literal adaptation, a reference return, a class-by-value
        // return, a unique_ptr return, and an expected<T, E> built by
        // the converting constructor.
        std::string case_name = "all_valid_return_shapes_still_produce_correct_values";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                           "class Box {\n"
                                           "public:\n"
                                           "    int v = 7;\n"
                                           "    virtual ~Box() { }\n"
                                           "};\n"
                                           "std::int64_t wide() { return 5; }\n"
                                           "Box& pick(Box& a) { return a; }\n"
                                           "Box make_box() {\n"
                                           "    Box b{};\n"
                                           "    return b;\n"
                                           "}\n"
                                           "std::unique_ptr<int> owned() { return std::make_unique<int>(9); }\n"
                                           "std::expected<int, int> ok_case() { return 4; }\n"
                                           "std::expected<void, int> void_case() { return {}; }\n"
                                           "void inner() { }\n"
                                           "void outer() { return inner(); }\n"
                                           "int main() {\n"
                                           "    int total = 0;\n"
                                           "    total = total + static_cast<int>(wide());\n"
                                           "    Box b{};\n"
                                           "    Box& r = pick(b);\n"
                                           "    total = total + r.v;\n"
                                           "    Box c = make_box();\n"
                                           "    total = total + c.v;\n"
                                           "    auto p = owned();\n"
                                           "    total = total + *p;\n"
                                           "    auto e = ok_case();\n"
                                           "    if (!e.has_value()) { return 100; }\n"
                                           "    total = total + e.value();\n"
                                           "    auto v = void_case();\n"
                                           "    if (!v.has_value()) { return 101; }\n"
                                           "    outer();\n"
                                           "    return total;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 32,
               case_name + ": expected 5 + 7 + 7 + 9 + 4 == 32 from the valid return shapes, got exit code " +
                   std::to_string(result.exit_code));
    }

    {
        // The store-boundary diagnostic used to end with "cast
        // expressions aren't implemented in this version yet". They are:
        // this program proves static_cast works at both boundaries and
        // computes the right answer.
        std::string case_name = "static_cast_satisfies_both_boundaries_at_runtime";
        cases_run++;
        RunResult result = compile_and_run("import std;\n"
                                           "std::int64_t widen(int x) {\n"
                                           "    return static_cast<std::int64_t>(x);\n"
                                           "}\n"
                                           "int main() {\n"
                                           "    int x = 21;\n"
                                           "    std::int64_t y = static_cast<std::int64_t>(x);\n"
                                           "    std::int64_t z = widen(x);\n"
                                           "    return static_cast<int>(y) + static_cast<int>(z);\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 42,
               case_name + ": expected 21 + 21 == 42 through explicit casts, got exit code " +
                   std::to_string(result.exit_code));
    }
}

void run_user_destructor_member_teardown_runtime_tests() {
    const std::string counters =
        "int dtors = 0;\n"
        "int order = 0;\n"
        "class Inner {\n"
        "  public:\n"
        "    Inner() { return; }\n"
        "    virtual ~Inner() { dtors = dtors + 1; order = order * 10 + this.tag; return; }\n"
        "    int tag = 0;\n"
        "};\n";
    const std::string counters_with_std = "import std;\n" + counters;

    {
        // The reported shape: a hand-written destructor body and two
        // class-typed members. Before the fix `dtors` stayed 0.
        std::string case_name = "user_written_destructor_destroys_its_own_members_in_reverse";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Outer {\n"
                                               "  public:\n"
                                               "    Outer() { this.a.tag = 1; this.b.tag = 2; return; }\n"
                                               "    virtual ~Outer() { order = order * 10 + 9; return; }\n"
                                               "    Inner a{};\n"
                                               "    Inner b{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    {\n"
                                               "        Outer o{};\n"
                                               "    }\n"
                                               "    if (dtors != 2) return 1;\n"
                                               "    if (order != 921) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected the user body first (9) then members in reverse (2 then 1), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // The `= default` spelling of the same class is the reference
        // behavior; the two must agree.
        std::string case_name = "user_written_and_defaulted_destructors_destroy_the_same_members";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class UserBody {\n"
                                               "  public:\n"
                                               "    UserBody() { this.a.tag = 1; this.b.tag = 2; return; }\n"
                                               "    virtual ~UserBody() { return; }\n"
                                               "    Inner a{};\n"
                                               "    Inner b{};\n"
                                               "};\n"
                                               "class Defaulted {\n"
                                               "  public:\n"
                                               "    Defaulted() { this.a.tag = 1; this.b.tag = 2; return; }\n"
                                               "    virtual ~Defaulted() = default;\n"
                                               "    Inner a{};\n"
                                               "    Inner b{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { UserBody u{}; }\n"
                                               "    int user_dtors = dtors;\n"
                                               "    int user_order = order;\n"
                                               "    dtors = 0;\n"
                                               "    order = 0;\n"
                                               "    { Defaulted d{}; }\n"
                                               "    if (user_dtors != dtors) return 1;\n"
                                               "    if (user_order != order) return 2;\n"
                                               "    if (dtors != 2) return 3;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected `~T() { }` and `~T() = default;` to destroy the same members, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Array-typed members are members too: #431 made array element
        // teardown expressible, but a user-written destructor never
        // reached it.
        std::string case_name = "user_written_destructor_destroys_array_typed_members";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Outer {\n"
                                               "  public:\n"
                                               "    Outer() { this.items[0].tag = 1; this.items[1].tag = 2; return; }\n"
                                               "    virtual ~Outer() { return; }\n"
                                               "    Inner items[2]{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Outer o{}; }\n"
                                               "    if (dtors != 2) return 1;\n"
                                               "    if (order != 21) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected both array elements destroyed back to front, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // The body has to still see its members alive: teardown is an
        // epilogue, not a prologue.
        std::string case_name = "user_written_destructor_body_sees_its_members_alive";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "int observed = 0;\n"
                                               "class Outer {\n"
                                               "  public:\n"
                                               "    Outer() { this.a.tag = 7; return; }\n"
                                               "    virtual ~Outer() { observed = this.a.tag; return; }\n"
                                               "    Inner a{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Outer o{}; }\n"
                                               "    if (observed != 7) return 1;\n"
                                               "    if (dtors != 1) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected the destructor body to read a live member before teardown, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Teardown belongs to the function epilogue, so every exit path
        // needs it -- an early `return;` as much as the last one.
        std::string case_name = "user_written_destructor_tears_down_on_an_early_return";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Outer {\n"
                                               "  public:\n"
                                               "    Outer() { this.a.tag = 1; return; }\n"
                                               "    virtual ~Outer() {\n"
                                               "        if (this.a.tag == 1) {\n"
                                               "            return;\n"
                                               "        }\n"
                                               "        return;\n"
                                               "    }\n"
                                               "    Inner a{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Outer o{}; }\n"
                                               "    if (dtors != 1) return 1;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected the early-return path to run member teardown, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // ...including the implicit `return;` synthesized when control
        // falls off the end of the body, which used to skip the whole
        // function epilogue.
        std::string case_name = "user_written_destructor_tears_down_when_control_falls_off_the_end";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Outer {\n"
                                               "  public:\n"
                                               "    Outer() { this.a.tag = 1; return; }\n"
                                               "    virtual ~Outer() { }\n"
                                               "    Inner a{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Outer o{}; }\n"
                                               "    if (dtors != 1) return 1;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected a destructor with no explicit `return;` to run member teardown, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // The same epilogue gap swallowed by-value class parameters of an
        // ordinary function whose body ends without `return;`.
        std::string case_name = "by_value_class_parameter_is_destroyed_when_control_falls_off_the_end";
        cases_run++;
        RunResult result = compile_and_run(counters_with_std +
                                               "void falls_off(Inner p) { }\n"
                                               "void explicit_return(Inner p) { return; }\n"
                                               "int main() {\n"
                                               "    Inner a{};\n"
                                               "    a.tag = 1;\n"
                                               "    falls_off(std::move(a));\n"
                                               "    if (dtors != 1) return 1;\n"
                                               "    Inner b{};\n"
                                               "    b.tag = 2;\n"
                                               "    explicit_return(std::move(b));\n"
                                               "    if (dtors != 2) return 2;\n"
                                               "    if (order != 12) return 3;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected a by-value class parameter destroyed on the fall-off-the-end path too, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Each class in a chain owns exactly its own members: derived
        // body, derived members, base body, base members. The base's
        // members must be destroyed once, by the base -- a derived
        // destructor that walked its flattened layout destroyed them a
        // second time.
        std::string case_name = "each_class_in_a_chain_destroys_only_its_own_members_once";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Base {\n"
                                               "  public:\n"
                                               "    Base() { this.bm.tag = 1; return; }\n"
                                               "    virtual ~Base() { order = order * 10 + 7; return; }\n"
                                               "    Inner bm{};\n"
                                               "};\n"
                                               "class Derived : public Base {\n"
                                               "  public:\n"
                                               "    Derived() { this.dm.tag = 2; return; }\n"
                                               "    ~Derived() override { order = order * 10 + 8; return; }\n"
                                               "    Inner dm{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Derived d{}; }\n"
                                               "    if (dtors != 2) return 1;\n"
                                               "    if (order != 8271) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected derived body, derived member, base body, base member exactly once each, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // The same, but with `= default` destructors -- the double
        // destruction of the base's members was a pre-existing defect of
        // the defaulted path, not something the user-written path
        // introduced.
        std::string case_name = "defaulted_derived_destructor_does_not_destroy_base_members_twice";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Base {\n"
                                               "  public:\n"
                                               "    Base() { this.bm.tag = 1; return; }\n"
                                               "    virtual ~Base() = default;\n"
                                               "    Inner bm{};\n"
                                               "};\n"
                                               "class Derived : public Base {\n"
                                               "  public:\n"
                                               "    Derived() { this.dm.tag = 2; return; }\n"
                                               "    ~Derived() override = default;\n"
                                               "    Inner dm{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { Derived d{}; }\n"
                                               "    if (dtors != 2) return 1;\n"
                                               "    if (order != 21) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected each member destroyed exactly once (2 and 1), got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Three levels, so a bug that merely skipped one level or
        // destroyed the immediate base's members twice still shows.
        std::string case_name = "three_level_chain_destroys_every_member_exactly_once_in_order";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class A {\n"
                                               "  public:\n"
                                               "    A() { this.am.tag = 1; return; }\n"
                                               "    virtual ~A() { order = order * 10 + 7; return; }\n"
                                               "    Inner am{};\n"
                                               "};\n"
                                               "class B : public A {\n"
                                               "  public:\n"
                                               "    B() { this.bm.tag = 2; return; }\n"
                                               "    ~B() override { order = order * 10 + 8; return; }\n"
                                               "    Inner bm{};\n"
                                               "};\n"
                                               "class C : public B {\n"
                                               "  public:\n"
                                               "    C() { this.cm.tag = 3; return; }\n"
                                               "    ~C() override { order = order * 10 + 9; return; }\n"
                                               "    Inner cm{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    { C c{}; }\n"
                                               "    if (dtors != 3) return 1;\n"
                                               "    if (order != 938271) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected C body, cm, B body, bm, A body, am exactly once each, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // Destruction through a base pointer takes the vtable thunk into
        // emit_destructor_chain_calls, which reaches the very same
        // per-class destructors -- so the members must come out the same
        // way.
        std::string case_name = "virtual_destruction_through_a_base_pointer_destroys_every_member_once";
        cases_run++;
        RunResult result = compile_and_run(counters +
                                               "class Base {\n"
                                               "  public:\n"
                                               "    Base() { this.bm.tag = 1; return; }\n"
                                               "    virtual ~Base() { order = order * 10 + 7; return; }\n"
                                               "    Inner bm{};\n"
                                               "};\n"
                                               "class Derived : public Base {\n"
                                               "  public:\n"
                                               "    Derived() { this.dm.tag = 2; return; }\n"
                                               "    ~Derived() override { order = order * 10 + 8; return; }\n"
                                               "    Inner dm{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    [[scpp::unsafe]] {\n"
                                               "        Base* p = new Derived();\n"
                                               "        delete p;\n"
                                               "    }\n"
                                               "    if (dtors != 2) return 1;\n"
                                               "    if (order != 8271) return 2;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected virtual destruction to reach every member exactly once, got exit " +
                   std::to_string(result.exit_code));
    }

    {
        // The practical consequence: a std::unique_ptr member of a class
        // with a hand-written destructor was never freed.
        std::string case_name = "user_written_destructor_frees_a_unique_ptr_member";
        cases_run++;
        RunResult result = compile_and_run(counters_with_std +
                                               "class Holder {\n"
                                               "  public:\n"
                                               "    Holder() { return; }\n"
                                               "    virtual ~Holder() { return; }\n"
                                               "    std::unique_ptr<Inner> p{};\n"
                                               "};\n"
                                               "int main() {\n"
                                               "    {\n"
                                               "        Holder h{};\n"
                                               "        h.p = std::make_unique<Inner>();\n"
                                               "    }\n"
                                               "    if (dtors != 1) return 1;\n"
                                               "    return 0;\n"
                                               "}\n",
                                           case_name);
        expect(result.exit_code == 0, case_name + ": expected the owned Inner to be freed exactly once, got exit " +
                                          std::to_string(result.exit_code));
    }

    {
        // A user-written *constructor* already initialized its members
        // (emit_constructor_member_initializers runs for every
        // constructor); this pins the symmetry the fix restores.
        std::string case_name = "user_written_constructor_still_initializes_its_members_exactly_once";
        cases_run++;
        RunResult result = compile_and_run("int ctors = 0;\n"
                                           "class Inner {\n"
                                           "  public:\n"
                                           "    Inner() { ctors = ctors + 1; this.tag = 5; return; }\n"
                                           "    virtual ~Inner() = default;\n"
                                           "    int tag = 0;\n"
                                           "};\n"
                                           "class Outer {\n"
                                           "  public:\n"
                                           "    Outer() { return; }\n"
                                           "    virtual ~Outer() { return; }\n"
                                           "    Inner a{};\n"
                                           "};\n"
                                           "int main() {\n"
                                           "    Outer o{};\n"
                                           "    if (ctors != 1) return 1;\n"
                                           "    if (o.a.tag != 5) return 2;\n"
                                           "    return 0;\n"
                                           "}\n",
                                           case_name);
        expect(result.exit_code == 0,
               case_name + ": expected a user-written constructor to construct its member once, got exit " +
                   std::to_string(result.exit_code));
    }
}

int main() {
    run_test_case_files();
    run_driver_single_test_case_files();
    run_error_location_tests();
    run_module_system_tests();
    run_concept_tests();
    run_generic_type_tests();
    run_generic_pack_deduction_tests();
    run_generic_function_overload_tests();
    run_reference_overload_forwarding_tests();
    run_functional_tests();
    run_thread_tests();
    run_global_scope_resolution_tests();
    run_nodiscard_tests();
    run_static_member_function_tests();
    run_default_argument_tests();
    run_static_local_lifetime_tests();
    run_loop_reborrow_release_tests();
    run_implicit_member_field_access_tests();
    run_random_tests();
    run_vector_tests();
    run_string_view_tests();
    run_size_t_keyword_tests();
    run_increment_decrement_tests();
    run_compound_assignment_tests();
    run_local_type_definition_tests();
    run_unordered_set_tests();
    run_expected_tests();
    run_optional_tests();
    run_member_lifetime_tests();
    run_reference_wrapper_tests();
    run_smart_pointer_nullptr_tests();
    run_subscripted_deref_tests();
    run_io_tests();
    run_enum_tests();
    run_switch_tests();
    test_compile_time_payload_plan_collects_exported_roots_and_helpers();
    run_sizeof_tests();
    run_storage_tests();
    run_local_constexpr_array_bound_tests();
    run_placement_new_tests();
    run_explicit_destructor_tests();
    run_consteval_tests();
    run_cli_extension_tests();
    run_brace_init_only_var_decl_tests();
    run_generic_type_construction_expression_tests();
    run_out_of_line_member_definition_tests();
    run_for_loop_tests();
    run_inheritance_constructor_and_destructor_tests();
    run_default_constructor_selection_tests();
    run_defaulted_special_member_tests();
    run_array_element_lifetime_runtime_tests();
    run_user_destructor_member_teardown_runtime_tests();
    run_return_type_checking_runtime_tests();
    run_generic_body_clone_fidelity_tests();
    run_scppm_payload_round_trip_tests();
    run_broken_module_dump_is_opt_in_tests();
    run_equality_operator_tests();
    test_compile_to_executable_returns_engaged_expected_on_success();
    test_compile_to_executable_returns_disengaged_expected_on_failure_without_throwing();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
