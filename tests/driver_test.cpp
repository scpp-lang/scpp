import scpp.driver;
import scpp.parser;
import scpp.movecheck;
import scpp.codegen;
import scpp.ast;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unordered_map>
#include <vector>

// SCPP_TEST_SOURCE_DIR is injected by CMake (see the driver_test target in
// the top-level CMakeLists.txt) and points at tests/test_source, so this
// binary finds its fixtures regardless of the working directory it's run
// from.
#ifndef SCPP_TEST_SOURCE_DIR
#error "SCPP_TEST_SOURCE_DIR must be defined by the build"
#endif
#ifndef SCPP_BINARY_PATH
#error "SCPP_BINARY_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_MODULE_PATH
#error "SCPP_STDLIB_STD_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STRING_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_STRING_WRAPPER_LIB_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_EXPECTED_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_EXPECTED_WRAPPER_LIB_PATH must be defined by the build"
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

std::uint32_t read_u32_le(const std::vector<unsigned char>& bytes, size_t offset) {
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

std::unordered_map<std::string, std::string> std_import_paths() {
    return {{"std", SCPP_STDLIB_STD_MODULE_PATH}};
}

std::vector<std::string> std_link_inputs() {
    return {SCPP_STDLIB_STRING_WRAPPER_LIB_PATH, SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH,
            SCPP_STDLIB_PRINT_WRAPPER_LIB_PATH, SCPP_STDLIB_EXPECTED_WRAPPER_LIB_PATH};
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
        scpp::Program parsed = scpp::parse(
            read_file(path_it->second), [this](const std::string& name) -> const scpp::Program& { return resolve(name); },
            [this](const std::string& key) -> scpp::Program { return resolve_partition(key); });
        auto [inserted, _] = cache_.emplace(module_name, std::move(parsed));
        return inserted->second;
    }

    scpp::Program resolve_partition(const std::string& key) {
        std::optional<std::string> path = infer_partition_path(key);
        if (!path.has_value()) throw std::runtime_error("unknown test partition '" + key + "'");
        return scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program& { return resolve(name); },
            [this](const std::string& nested_key) -> scpp::Program { return resolve_partition(nested_key); });
    }

private:
    std::optional<std::string> infer_partition_path(const std::string& key) const {
        size_t colon = key.find(':');
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

scpp::Program parse_with_std_imports(std::string_view source) {
    TestModuleCache cache(std_import_paths());
    return scpp::parse(
        source, [&cache](const std::string& name) -> const scpp::Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> scpp::Program { return cache.resolve_partition(key); });
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
        size_t n;
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
    size_t name_pos = dwarfdump_result.stdout_text.find(needle);
    expect(name_pos != std::string::npos,
           case_name + ": expected DWARF entry for variable '" + variable_name + "', got '" +
               dwarfdump_result.stdout_text + "'");
    if (name_pos == std::string::npos) return;
    size_t entry_begin = dwarfdump_result.stdout_text.rfind("DW_TAG_variable", name_pos);
    expect(entry_begin != std::string::npos,
           case_name + ": expected DW_TAG_variable for '" + variable_name + "', got '" +
               dwarfdump_result.stdout_text + "'");
    if (entry_begin == std::string::npos) return;
    size_t entry_end = dwarfdump_result.stdout_text.find("DW_TAG_", name_pos + needle.size());
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
    size_t name_pos = dwarfdump_result.stdout_text.find(needle);
    expect(name_pos != std::string::npos,
           case_name + ": expected DWARF entry named '" + entry_name + "', got '" + dwarfdump_result.stdout_text + "'");
    if (name_pos == std::string::npos) return;
    size_t entry_begin = dwarfdump_result.stdout_text.rfind(tag_name, name_pos);
    expect(entry_begin != std::string::npos,
           case_name + ": expected " + tag_name + " for '" + entry_name + "', got '" + dwarfdump_result.stdout_text + "'");
    if (entry_begin == std::string::npos) return;
    size_t entry_end = dwarfdump_result.stdout_text.find("DW_TAG_", name_pos + needle.size());
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
RunResult compile_and_run(std::string_view source, const std::string& case_name) {
    std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
    scpp::compile_to_executable(source, exe_path.string(), std_link_inputs(), std_import_paths());

    FILE* pipe = popen(exe_path.string().c_str(), "r");
    std::string output;
    if (pipe != nullptr) {
        char buffer[256];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            output.append(buffer, n);
        }
    }
    int status = pipe != nullptr ? pclose(pipe) : -1;

    std::filesystem::remove(exe_path);
    return RunResult{WEXITSTATUS(status), output};
}

// A `<name>.expected` file's first line is the expected exit code; anything
// after the first newline is the expected stdout, compared exactly.
struct ExpectedResult {
    int exit_code;
    std::string stdout_text;
};

ExpectedResult parse_expected(const std::string& content) {
    size_t newline = content.find('\n');
    std::string exit_code_line = newline == std::string::npos ? content : content.substr(0, newline);
    std::string stdout_text = newline == std::string::npos ? "" : content.substr(newline + 1);
    return ExpectedResult{std::stoi(exit_code_line), stdout_text};
}

// Runs every `<name>.scpp` case file under SCPP_TEST_SOURCE_DIR against its
// paired `<name>.expected` file (see parse_expected). Adding a new test case
// is just dropping in 2 new files -- no changes to this file or a rebuild of
// the test harness are needed, just re-running the already-built binary.
void run_test_case_files() {
    std::filesystem::path dir(SCPP_TEST_SOURCE_DIR);
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

        try {
            RunResult result = compile_and_run(read_file(source_path), case_name);
            expect(result.exit_code == expected.exit_code, case_name + ": expected exit code " +
                                                                std::to_string(expected.exit_code) + ", got " +
                                                                std::to_string(result.exit_code));
            expect(result.stdout_text == expected.stdout_text, case_name + ": expected stdout '" +
                                                                    expected.stdout_text + "', got '" +
                                                                    result.stdout_text + "'");
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
        }
    }
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
        try {
            scpp::parse(c.source);
            expect(false, c.name + ": expected a ParseError, none was thrown");
        } catch (const scpp::ParseError& e) {
            expect(e.loc.is_known(), c.name + ": ParseError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
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
        try {
           scpp::Program program = parse_with_std_imports(c.source);
           scpp::monomorphize_generics(program);
           scpp::check_moves(program);
           expect(false, c.name + ": expected a DataflowError, none was thrown");
        } catch (const scpp::DataflowError& e) {
           expect(e.loc.is_known(), c.name + ": DataflowError has no location");
           expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
        }
    }

    std::vector<Case> codegen_cases = {
        {"bool_int_mismatch", "int main() {\n    bool b = 5;\n    return 0;\n}\n", 2},
    };
    for (const Case& c : codegen_cases) {
        cases_run++;
        try {
            scpp::Program program = scpp::parse(c.source);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
            expect(false, c.name + ": expected a CodegenError, none was thrown");
        } catch (const scpp::CodegenError& e) {
            expect(e.loc.is_known(), c.name + ": CodegenError has no location");
            expect(e.loc.line == c.expected_line, c.name + ": expected line " +
                                                       std::to_string(c.expected_line) + ", got " +
                                                       std::to_string(e.loc.line));
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
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"mathlib", lib_path.string()}});
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "36\n37\n", case_name + ": expected stdout '36\\n37\\n', got '" + output + "'");
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
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
        bool threw = false;
        try {
            scpp::Program lib_program = scpp::parse(read_file(lib_path));
            scpp::Program program = scpp::parse(
                main_source, [&lib_program](const std::string&) -> const scpp::Program& { return lib_program; });
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected calling a module-private function to fail");
        std::filesystem::remove(lib_path);
    }

    // Importing a module whose own module declaration doesn't match the
    // requested name is rejected (a mismatched --import name=path).
    {
        std::string case_name = "module_name_mismatch_is_rejected";
        cases_run++;
        std::filesystem::path lib_path = write_temp_file(case_name, "lib", "export module actuallib;\n");
        bool threw = false;
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable("import mathlib;\nint main() { return 0; }\n", exe_path.string(), {},
                                         {{"mathlib", lib_path.string()}});
            std::filesystem::remove(exe_path);
        } catch (const scpp::DriverError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected a DriverError for a module name mismatch");
        std::filesystem::remove(lib_path);
    }

    // Importing a module with no corresponding --import mapping at all
    // is rejected with a clear error, not a crash.
    {
        std::string case_name = "missing_import_mapping_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable("import nonexistent;\nint main() { return 0; }\n", exe_path.string());
            std::filesystem::remove(exe_path);
        } catch (const scpp::DriverError&) {
            threw = true;
        }
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
        try {
            scpp::compile_to_executable("import mathlib;\nint main() { return mathlib::value(); }\n", exe_path.string(), {},
                                        {}, /*static_link=*/false, {module_dir.string()});
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            expect(run.exit_code == 17,
                   case_name + ": expected exit code 17, got " + std::to_string(run.exit_code));
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
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
        try {
            scpp::compile_to_executable("import mathlib;\nint main() { return mathlib::value(); }\n", exe_path.string(), {},
                                        {}, /*static_link=*/false, {first_dir.string(), second_dir.string()});
            RunResult run = run_command_capture(exe_path.string() + " 2>&1");
            expect(run.exit_code == 11,
                   case_name + ": expected first -I directory to win, got exit code " +
                       std::to_string(run.exit_code));
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
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
        bool threw = false;
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable("import a;\nint main() { return 0; }\n", exe_path.string(), {},
                                         {{"a", a_path.string()}, {"b", b_path.string()}});
            std::filesystem::remove(exe_path);
        } catch (const scpp::DriverError&) {
            threw = true;
        }
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
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "36\n45\n", case_name + ": expected stdout '36\\n45\\n', got '" + output + "'");
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
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
        bool threw = false;
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
            std::filesystem::remove(exe_path);
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::DataflowError&) {
            threw = true;
        }
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
        bool threw = false;
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"mathlib", lib_path.string()}, {"mathlib:trig", trig_path.string()}});
            std::filesystem::remove(exe_path);
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::DataflowError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected sin_deg_approx to stay invisible (plain import :part; never "
                                   "re-exports)");
        std::filesystem::remove(trig_path);
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
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"a", a_path.string()}, {"b", b_path.string()}});
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "42\n43\n", case_name + ": expected stdout '42\\n43\\n', got '" + output + "'");
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
        }
        std::filesystem::remove(a_path);
        std::filesystem::remove(b_path);
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
        try {
            std::filesystem::path exe_path =
                std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_exe");
            scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                         {{"mathlib", mathlib_path.string()},
                                          {"mathlib:base", base_path.string()},
                                          {"mathlib:random", random_path.string()}});
            FILE* pipe = popen(exe_path.string().c_str(), "r");
            std::string output;
            if (pipe != nullptr) {
                char buffer[256];
                size_t n;
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
            }
            int status = pipe != nullptr ? pclose(pipe) : -1;
            std::filesystem::remove(exe_path);
            expect(WEXITSTATUS(status) == 0, case_name + ": expected exit code 0");
            expect(output == "42\n43\n", case_name + ": expected stdout '42\\n43\\n', got '" + output + "'");
        } catch (const std::exception& e) {
            expect(false, case_name + ": threw an exception: " + std::string(e.what()));
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
            try {
                std::filesystem::path exe_path = std::filesystem::temp_directory_path() /
                                                  ("scpp_driver_test_" + case_name + "_indirect_exe");
                scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                             {{"a", a_path.string()}, {"b", b_path.string()}});
                FILE* pipe = popen(exe_path.string().c_str(), "r");
                std::string output;
                if (pipe != nullptr) {
                    char buffer[256];
                    size_t n;
                    while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) output.append(buffer, n);
                }
                int status = pipe != nullptr ? pclose(pipe) : -1;
                std::filesystem::remove(exe_path);
                expect(WEXITSTATUS(status) == 0, case_name + " (indirect): expected exit code 0");
                expect(output == "43\n", case_name + " (indirect): expected stdout '43\\n', got '" + output + "'");
            } catch (const std::exception& e) {
                expect(false, case_name + " (indirect): threw an exception: " + std::string(e.what()));
            }
        }

        // The direct call (relying on transitive visibility through a
        // private import) must be rejected.
        {
            cases_run++;
            std::string main_source = "import b;\nint main() { print_int(a::value()); return 0; }\n";
            bool threw = false;
            try {
                std::filesystem::path exe_path = std::filesystem::temp_directory_path() /
                                                  ("scpp_driver_test_" + case_name + "_direct_exe");
                scpp::compile_to_executable(main_source, exe_path.string(), /*extra_link_inputs=*/{},
                                             {{"a", a_path.string()}, {"b", b_path.string()}});
                std::filesystem::remove(exe_path);
            } catch (const scpp::CodegenError&) {
                threw = true;
            } catch (const scpp::DataflowError&) {
                threw = true;
            }
            expect(threw, case_name + " (direct): expected a::value() to stay invisible (private import is "
                                       "non-transitive)");
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
            "    Circle c;\n"
            "    return print_area(c);\n"
            "}\n";
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
        std::string source =
            "template<typename T>\n"
            "concept Describable = requires(const T& t) {\n"
            "    { t.magnitude() } -> std::same_as<int>;\n"
            "};\n"
            "class NoMagnitude {\n"
            "public:\n"
            "    NoMagnitude(int v) { this.value = v; return; }\n"
            "private:\n"
            "    int value;\n"
            "};\n"
            "template<typename T>\n"
            "class Vec {\n"
            "    T item;\n"
            "public:\n"
            "    Vec(const T& x) { this.item = x; return; }\n"
            "    int describe() const requires Describable<T> {\n"
            "        return this.item.magnitude();\n"
            "    }\n"
            "};\n"
            "int main() {\n"
            "    NoMagnitude n(1);\n"
            "    Vec<NoMagnitude> vn(n);\n"
            "    return vn.describe();\n"
            "}\n";
        std::string case_name = "generic_class_constrained_method_unsatisfying_type_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> b(\"hi\");\n"
            "    return 0;\n"
            "}\n";
        std::string case_name = "variadic_generic_instantiation_clones_constructor";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    int size() const { return 10; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    int size() const { return 50; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> b;\n"
            "    return b.size() - 50;\n"
            "}\n";
        std::string case_name = "variadic_generic_instantiation_clones_methods";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    int value;\n"
            "    consteval Box(const char* s) {\n"
            "        this->value = 7;\n"
            "        return;\n"
            "    }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "\n"
            "int main() {\n"
            "    Box<> b(\"hi\");\n"
            "    return b.get() - 7;\n"
            "}\n";
        std::string case_name = "variadic_empty_pack_base_case_clones_fields";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    return invoke(h, 19, 23) - 42;\n"
            "}\n";
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    Holder<int(int, int)> h;\n"
            "    h.fn_ = add;\n"
            "    return invoke<int>(h, 20, 22) - 42;\n"
            "}\n";
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected a mismatched earlier dependent parameter to be rejected");
    }

    {
        std::string case_name = "variadic_generic_parameter_is_checked_after_later_pack_deduction";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<> class Box<> {};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {};\n"
            "\n"
            "template<typename... Args>\n"
            "int use(const Box<Args...>& fmt, Args&&... args) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Box<int> ok;\n"
            "    return use(ok, 1) - 42;\n"
            "}\n";
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(!threw, case_name + ": expected Box<int> plus one later arg to satisfy Box<Args...>");
    }

    {
        std::string case_name = "variadic_generic_parameter_rejects_mismatch_after_later_pack_deduction";
        cases_run++;
        std::string source =
            "template<typename... Args> class Box;\n"
            "\n"
            "template<> class Box<> {};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {};\n"
            "\n"
            "template<typename... Args>\n"
            "int use(const Box<Args...>& fmt, Args&&... args) {\n"
            "    return 42;\n"
            "}\n"
            "\n"
            "int main() {\n"
            "    Box<int, bool> bad;\n"
            "    return use(bad, 1) - 42;\n"
            "}\n";
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    consteval Box(const char* s) { return; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
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
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
            "    int value;\n"
            "    consteval Box(const char* s) {\n"
            "        this->value = 7;\n"
            "        return;\n"
            "    }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "\n"
            "template<typename Head, typename... Tail>\n"
            "class Box<Head, Tail...> : private Box<Tail...> {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Box(const char* s) {\n"
            "        this->value = 9;\n"
            "        return;\n"
            "    }\n"
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
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(source);
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(!threw, case_name + ": expected unmatched generic helpers to defer to the nongeneric overload");
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
        "    MoveOnlyAdder(std::unique_ptr<int> value) { this->value = std::move(value); return; }\n"
        "    int call(int x) const { return x + *this->value; }\n"
        "};\n"
        "int main() {\n"
        "    std::function<int(int) const> f(MoveOnlyAdder(std::make_unique<int>(5)));\n"
        "    return f(7);\n"
        "}\n";
    bool threw = false;
    try {
        scpp::Program program = parse_with_std_imports(source);
        scpp::monomorphize_generics(program);
        scpp::check_moves(program);
        scpp::Codegen codegen("test_module");
        codegen.generate(program);
    } catch (const scpp::DataflowError&) {
        threw = true;
    } catch (const scpp::CodegenError&) {
        threw = true;
    } catch (const scpp::ParseError&) {
        threw = true;
    }
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
    bool threw = false;
    try {
        scpp::Program program = parse_with_std_imports(source);
        scpp::monomorphize_generics(program);
        scpp::check_moves(program);
        scpp::Codegen codegen("test_module");
        codegen.generate(program);
    } catch (const scpp::DataflowError&) {
        threw = true;
    } catch (const scpp::CodegenError&) {
        threw = true;
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, case_name + ": expected std::jthread to reject a reference-capturing closure target");
}

void test_compile_time_payload_plan_collects_exported_roots_and_helpers() {
    std::string case_name = "compile_time_payload_plan_collects_exported_roots_and_helpers";
    cases_run++;
    scpp::Program program = scpp::parse(
        "export module math;\n"
        "namespace math {\n"
        "    class Helper { public: constexpr Helper(int v) { value = v; } int value; };\n"
        "    int helper_value(const Helper& h) { return h.value; }\n"
        "    export constexpr int answer() { Helper h(42); return helper_value(h); }\n"
        "}\n");
    scpp::CompileTimePayloadPlan plan = scpp::plan_compile_time_payload(program);
    expect(plan.format_version == scpp::SCPPM_COMPILE_TIME_AST_VERSION,
           case_name + ": expected compile-time payload format version 1");
    expect(std::find(plan.root_function_names.begin(), plan.root_function_names.end(), "math::answer") !=
               plan.root_function_names.end(),
           case_name + ": expected exported constexpr function root");
    expect(std::find(plan.reachable_function_names.begin(), plan.reachable_function_names.end(), "math::helper_value") !=
               plan.reachable_function_names.end(),
           case_name + ": expected private helper function to be reachable");
    expect(std::find(plan.reachable_function_names.begin(), plan.reachable_function_names.end(), "math::Helper_new") !=
               plan.reachable_function_names.end(),
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
        scpp::compile_to_executable(
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
            "    Pair pair;\n"
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
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected sizeof runtime checks to exit 0, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "sizeof_is_unevaluated_for_movecheck";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::current_path() / "sizeof_is_unevaluated_for_movecheck_exe";
        scpp::compile_to_executable(
            "import std;\n"
            "int consume(std::unique_ptr<int> p) {\n"
            "    return 0;\n"
            "}\n"
            "int main() {\n"
            "    std::unique_ptr<int> p;\n"
            "    int n = (int)sizeof(std::move(p));\n"
            "    return consume(std::move(p)) + n - n;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "struct Tiny {\n"
            "    char x;\n"
            "};\n"
            "consteval int answer() {\n"
            "    Tiny t;\n"
            "    return (int)sizeof(Tiny) + (int)sizeof(t);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 2;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected consteval sizeof folding to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_storage_tests() {
    {
        std::string case_name = "storage_for_uses_max_size_and_alignment";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "storage_for_uses_max_size_and_alignment_exe";
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    std::storage_for<int, long> slot;\n"
            "    int payload_size() const { return (int)sizeof(this->slot); }\n"
            "};\n"
            "struct Holder {\n"
            "    char tag;\n"
            "    std::storage_for<int, long> slot;\n"
            "    char tail;\n"
            "};\n"
            "int main() {\n"
            "    Box box;\n"
            "    if (box.payload_size() != 8) return 1;\n"
            "    if ((int)sizeof(std::storage_for<int, long>) != 8) return 2;\n"
            "    if ((int)sizeof(Holder) != 24) return 3;\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected aligned storage layout checks to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "storage_for_accepts_user_defined_candidate_types";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "storage_for_accepts_user_defined_candidate_types_exe";
        scpp::compile_to_executable(
            "class Widget {\n"
            "public:\n"
            "    char c;\n"
            "    long value;\n"
            "};\n"
            "struct Wrapper {\n"
            "    char lead;\n"
            "    std::storage_for<Widget, int> storage;\n"
            "};\n"
            "int main() {\n"
            "    if ((int)sizeof(std::storage_for<Widget, int>) != 16) return 1;\n"
            "    if ((int)sizeof(Wrapper) != 24) return 2;\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected user-defined-type storage checks to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_placement_new_tests() {
    {
        std::string case_name = "placement_new_constructs_scalar_in_storage";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "placement_new_constructs_scalar_in_storage_exe";
        scpp::compile_to_executable(
            "int main() {\n"
            "    std::storage_for<int> slot;\n"
            "    [[scpp::unsafe]] {\n"
            "        int* p = new ((int*)&slot) int(7);\n"
            "        return *p - 7;\n"
            "    }\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int value;\n"
            "    Box(int v) { this->value = v; return; }\n"
            "    int get() const { return this->value; }\n"
            "};\n"
            "int main() {\n"
            "    std::storage_for<Box> slot;\n"
            "    [[scpp::unsafe]] {\n"
            "        Box* p = new ((Box*)&slot) Box(9);\n"
            "        return p->get() - 9;\n"
            "    }\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int* out;\n"
            "    Box(int* p) { this->out = p; return; }\n"
            "    ~Box() { [[scpp::unsafe]] { *this->out = 9; } return; }\n"
            "};\n"
            "int main() {\n"
            "    int result = 0;\n"
            "    std::storage_for<Box> slot;\n"
            "    [[scpp::unsafe]] {\n"
            "        Box* p = new ((Box*)&slot) Box(&result);\n"
            "        p->~Box();\n"
            "    }\n"
            "    return result - 9;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected explicit destructor call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "object_form_explicit_destructor_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(
                "class Box { public: ~Box() { return; } }; int main() { Box b; [[scpp::unsafe]] { b.~Box(); } return 0; }");
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected object-form explicit destructor call to be rejected");
    }
}

void run_consteval_tests() {
    {
        std::string case_name = "consteval_folds_recursive_constexpr_helper";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "consteval_folds_recursive_constexpr_helper_exe";
        scpp::compile_to_executable(
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
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Box(int v) {\n"
            "        this->value = v;\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Box b(42);\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 42;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Box(const char* text) {\n"
            "        this->value = 17;\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "int take(Box b) {\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return take(\"hi\") - 17;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Box {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Box(const char* text) {\n"
            "        this->value = 23;\n"
            "        return;\n"
            "    }\n"
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
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "consteval int add_40(int x) {\n"
            "    return x + 40;\n"
            "}\n"
            "consteval int route(int x) {\n"
            "    return add_40(x);\n"
            "}\n"
            "int main() {\n"
            "    return route(2) - 42;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "constexpr int size1(const char* s) {\n"
            "    return 7;\n"
            "}\n"
            "class Box {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Box(const char* s) {\n"
            "        this->value = size1(s);\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Box b(\"hi\");\n"
            "    return b.value;\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 7;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Counter {\n"
            "public:\n"
            "    int value;\n"
            "    consteval Counter(int v) {\n"
            "        this->value = v;\n"
            "        return;\n"
            "    }\n"
            "    consteval void bump() {\n"
            "        this->value = this->value + 1;\n"
            "        return;\n"
            "    }\n"
            "    constexpr int get() const {\n"
            "        return this->value;\n"
            "    }\n"
            "};\n"
            "consteval int answer() {\n"
            "    Counter c(6);\n"
            "    c.bump();\n"
            "    return c.get();\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 7;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "class Helper {\n"
            "public:\n"
            "    consteval Helper(const char* s, int i) { return; }\n"
            "};\n"
            "class Outer {\n"
            "public:\n"
            "    consteval Outer(const char* s) {\n"
            "        Helper h(s, 0);\n"
            "        return;\n"
            "    }\n"
            "};\n"
            "int main() {\n"
            "    Outer o(\"x\");\n"
            "    return 0;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "template<typename... Ts> class TagList;\n"
            "template<>\n"
            "class TagList<> {\n"
            "public:\n"
            "    TagList() { return; }\n"
            "};\n"
            "template<typename Head, typename... Tail>\n"
            "class TagList<Head, Tail...> : private TagList<Tail...> {\n"
            "public:\n"
            "    TagList() { return; }\n"
            "};\n"
            "consteval int take(TagList<> tags) {\n"
            "    return 41;\n"
            "}\n"
            "consteval int answer() {\n"
            "    TagList<int, bool> tags;\n"
            "    return take(tags);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 41;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "template<typename... Ts> class TagList;\n"
            "template<>\n"
            "class TagList<> {\n"
            "public:\n"
            "    TagList() { return; }\n"
            "};\n"
            "template<typename Head, typename... Tail>\n"
            "class TagList<Head, Tail...> : private TagList<Tail...> {\n"
            "public:\n"
            "    TagList() { return; }\n"
            "};\n"
            "consteval int take_ref(const TagList<>& tags) {\n"
            "    return 41;\n"
            "}\n"
            "consteval int answer() {\n"
            "    TagList<int, bool> tags;\n"
            "    return take_ref(tags);\n"
            "}\n"
            "int main() {\n"
            "    return answer() - 41;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected derived-to-base consteval ref call to exit 0, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "consteval_rejects_runtime_only_call";
        cases_run++;
        bool threw = false;
        try {
            scpp::compile_to_executable(
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
                std_link_inputs(), std_import_paths());
        } catch (const scpp::DriverError& error) {
            threw = std::string(error.what()).find("immediate evaluation may only call constexpr/consteval functions") !=
                    std::string::npos;
        }
        expect(threw, case_name + ": expected clear runtime-only immediate-call rejection");
    }

    {
        std::string case_name = "if_consteval_selects_compile_time_and_runtime_branches";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "if_consteval_selects_compile_time_and_runtime_branches_exe";
        scpp::compile_to_executable(
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
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
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
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
            "constexpr int plus_one(int x) {\n"
            "    return x + 1;\n"
            "}\n"
            "int main() {\n"
            "    constexpr int base = 4;\n"
            "    constexpr int total = plus_one(base);\n"
            "    return total;\n"
            "}\n",
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 5,
               case_name + ": expected constexpr local result 5, got " + std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "constexpr_local_rejects_runtime_initializer";
        cases_run++;
        bool threw = false;
        try {
            scpp::compile_to_executable(
                "int main() {\n"
                "    int runtime = 4;\n"
                "    constexpr int total = runtime + 1;\n"
                "    return total;\n"
                "}\n",
                (std::filesystem::current_path() / "constexpr_local_rejects_runtime_initializer_exe").string(),
                std_link_inputs(), std_import_paths());
        } catch (const scpp::DriverError& error) {
            threw = std::string(error.what()).find("identifier 'runtime' is not available") != std::string::npos;
        }
        expect(threw, case_name + ": expected constexpr local to reject runtime-only initializer");
    }

    {
        std::string case_name = "if_consteval_propagates_required_constant_evaluation_into_callees";
        cases_run++;
        std::filesystem::path exe_path =
            std::filesystem::current_path() / "if_consteval_propagates_required_constant_evaluation_into_callees_exe";
        scpp::compile_to_executable(
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
            exe_path.string(), std_link_inputs(), std_import_paths());
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
        scpp::compile_to_executable(
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
            exe_path.string(), std_link_inputs(), std_import_paths());
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 45,
               case_name + ": expected non-selected runtime-only branch to be ignored, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove(exe_path);
    }

    {
        std::string case_name = "required_constant_evaluation_rejects_user_defined_destructor_execution";
        cases_run++;
        bool threw = false;
        try {
            scpp::compile_to_executable(
                "class NeedsDrop {\n"
                "public:\n"
                "    int value;\n"
                "    constexpr NeedsDrop(int x) {\n"
                "        this->value = x;\n"
                "        return;\n"
                "    }\n"
                "    ~NeedsDrop() {\n"
                "        return;\n"
                "    }\n"
                "};\n"
                "constexpr int make_value() {\n"
                "    NeedsDrop box(42);\n"
                "    return box.value;\n"
                "}\n"
                "int main() {\n"
                "    constexpr int value = make_value();\n"
                "    return value;\n"
                "}\n",
                (std::filesystem::current_path() /
                 "required_constant_evaluation_rejects_user_defined_destructor_execution_exe")
                    .string(),
                std_link_inputs(), std_import_paths());
        } catch (const scpp::DriverError& error) {
            threw = std::string(error.what()).find("cannot execute user-defined destructor of 'NeedsDrop'") !=
                    std::string::npos;
        }
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
                        "        Secret s;\n"
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
            expect(interface_bytes.size() >= static_cast<size_t>(12 + interface_length + 8),
                   case_name + ": expected payload bytes after embedded interface source");
            std::string embedded_source(interface_bytes.begin() + 12, interface_bytes.begin() + 12 + interface_length);
            expect(embedded_source.find("return add_bonus(value, s);") == std::string::npos,
                   case_name + ": generic function body should be stripped from interface source");
            expect(embedded_source.find("return value + s.value;") == std::string::npos,
                   case_name + ": private helper generic body should be stripped from interface source");
            std::uint32_t payload_length = read_u32_le(interface_bytes, 12 + interface_length);
            expect(payload_length > 8, case_name + ": expected non-trivial structured payload length");
            if (interface_bytes.size() >= static_cast<size_t>(16 + interface_length + payload_length)) {
                size_t payload_offset = 16 + interface_length;
                expect(std::string(interface_bytes.begin() + payload_offset, interface_bytes.begin() + payload_offset + 4) ==
                           "SAST",
                       case_name + ": expected structured payload magic");
                expect(read_u32_le(interface_bytes, payload_offset + 4) == scpp::SCPPM_COMPILE_TIME_AST_VERSION,
                       case_name + ": expected structured payload version 1");
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
                        "        consteval Box(const char* s) { return; }\n"
                        "        int mark() const { return 7; }\n"
                        "    };\n"
                        "\n"
                        "    export template<typename Head, typename... Tail>\n"
                        "    class Box<Head, Tail...> : private helper::Box<Tail...> {\n"
                        "    public:\n"
                        "        consteval Box(const char* s) { helper::Box<Tail...> tail(s); return; }\n"
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
                        "    helper::Box<int, bool> box(\"ok\");\n"
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
        expect(result.stdout_text.find("must use the .scpp extension") != std::string::npos,
               case_name + ": expected extension error, got '" + result.stdout_text + "'");
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
        std::string case_name = "cli_import_std_works_after_relocation";
        std::filesystem::path bundle_root = std::filesystem::current_path() / "cli_import_std_works_after_relocation_bundle";
        std::filesystem::path bundle_build_dir = bundle_root / "build";
        std::filesystem::path bundle_build_stdlib_dir = bundle_build_dir / "stdlib";
        std::filesystem::path relocated_scpp = bundle_build_dir / "scpp";
        std::filesystem::path source_path = bundle_root / "main.scpp";
        std::filesystem::path exe_path = bundle_root / "app";
        cases_run++;
        std::filesystem::remove_all(bundle_root);
        std::filesystem::create_directories(bundle_build_dir);
        std::filesystem::copy_file(SCPP_BINARY_PATH, relocated_scpp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_BINARY_PATH).parent_path() / "stdlib", bundle_build_stdlib_dir,
                              std::filesystem::copy_options::recursive);
        write_text_file(source_path,
                        "import std;\n"
                        "int main() {\n"
                        "    std::string s(\"relocated\");\n"
                        "    return s.length();\n"
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
        std::filesystem::path install_stdlib_dir = install_root / "share" / "scpp" / "stdlib";
        std::filesystem::path installed_scpp = install_bin_dir / "scpp";
        std::filesystem::path source_path = install_root / "main.scpp";
        std::filesystem::path exe_path = install_root / "app";
        cases_run++;
        std::filesystem::remove_all(install_root);
        std::filesystem::create_directories(install_bin_dir);
        std::filesystem::create_directories(install_stdlib_dir);
        std::filesystem::copy_file(SCPP_BINARY_PATH, installed_scpp, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_STDLIB_STD_MODULE_PATH).parent_path(), install_stdlib_dir,
                              std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy(std::filesystem::path(SCPP_BINARY_PATH).parent_path() / "stdlib", install_stdlib_dir,
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
        std::string case_name = "cli_project_build_builds_manifest_bin";
        std::filesystem::path root = std::filesystem::current_path() / "cli_project_build_builds_manifest_bin";
        std::filesystem::path src_dir = root / "src";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "hello" / "hello";
        std::filesystem::path helper_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "hello" / "modules" / "helper.scppm";
        std::filesystem::path helper_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "hello" / "archives" / "libhelper.scppa";
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
                        "root = \"src/main.scpp\"\n"
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
        std::string case_name = "cli_project_build_bare_scpp_aliases_build";
        std::filesystem::path root = std::filesystem::current_path() / "cli_project_build_bare_scpp_aliases_build";
        std::filesystem::path src_dir = root / "src";
        std::filesystem::path exe_path =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "app" / "app";
        std::filesystem::path lib_iface =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "app" / "modules" / "mylib.scppm";
        std::filesystem::path lib_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "app" / "archives" / "libmylib.scppa";
        cases_run++;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(src_dir);
        write_text_file(root / "scpp.toml",
                        "manifest-version = 1\n"
                        "\n"
                        "[package]\n"
                        "name = \"app\"\n"
                        "\n"
                        "[lib]\n"
                        "root = \"src/mylib.scpp\"\n"
                        "sources = [\"src/**/*.scpp\"]\n"
                        "\n"
                        "[[bin]]\n"
                        "name = \"app\"\n"
                        "root = \"src/main.scpp\"\n"
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
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "app" / "app";
        std::filesystem::path tls_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "tls" / "archives" / "libtls.scppa";
        std::filesystem::path net_archive =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "net" / "archives" / "libnet.scppa";
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
                        "[lib]\n"
                        "root = \"src/tls.scpp\"\n"
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
                        "[lib]\n"
                        "root = \"src/net.scpp\"\n"
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
                        "root = \"src/main.scpp\"\n"
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
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "rootapp" / "rootapp";
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
                        "root = \"src/main.scpp\"\n"
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
                        "[lib]\n"
                        "root = \"src/tls.scpp\"\n"
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
        std::filesystem::path build_root = root / ".scpp" / "build" / scpp::host_target_triple() / "dev";
        std::filesystem::path dep_archive = build_root / "dep" / "archives" / "libdep.scppa";
        std::filesystem::path dep_interface = build_root / "dep" / "modules" / "dep.scppm";
        std::filesystem::path app_object = build_root / "app" / "objects" / "app" / "root.o";
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
                        "[lib]\n"
                        "root = \"src/dep.scpp\"\n"
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
                        "root = \"src/main.scpp\"\n"
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
        std::string case_name = "cli_native_links_propagate_transitively";
        std::filesystem::path root = std::filesystem::current_path() / "cli_native_links_propagate_transitively";
        std::filesystem::path trig_dir = root / "trig";
        std::filesystem::path net_dir = root / "net";
        std::filesystem::path app_dir = root / "app";
        std::filesystem::path app_exe =
            root / ".scpp" / "build" / scpp::host_target_triple() / "dev" / "app" / "app";
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
                        "[lib]\n"
                        "root = \"src/trig.scpp\"\n"
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
                        "[lib]\n"
                        "root = \"src/net.scpp\"\n"
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
                        "root = \"src/main.scpp\"\n"
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
        std::string case_name = "enum_class_same_type_comparison_and_casts_compile_and_run";
        cases_run++;
        RunResult result = compile_and_run(
            "enum class Color : uint8_t { red = 1, green = 2, blue = 3 };\n"
            "int main() {\n"
            "    Color color = static_cast<Color>((uint8_t)2);\n"
            "    if (color != Color::green) return 1;\n"
            "    uint8_t raw = static_cast<uint8_t>(Color::blue);\n"
            "    return (int)raw - 3;\n"
            "}\n",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }

    {
        std::string case_name = "enum_class_cross_type_comparison_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(
                "enum class Color { red };\n"
                "enum class Shape { red };\n"
                "int main() { return Color::red == Shape::red; }\n");
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected cross-enum comparison to be rejected");
    }

    {
        std::string case_name = "enum_class_implicit_int_to_enum_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(
                "enum class Color { red };\n"
                "int main() { Color color = 1; return 0; }\n");
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
        expect(threw, case_name + ": expected implicit int-to-enum conversion to be rejected");
    }

    {
        std::string case_name = "enum_class_implicit_enum_to_int_is_rejected";
        cases_run++;
        bool threw = false;
        try {
            scpp::Program program = scpp::parse(
                "enum class Color { red };\n"
                "int main() { int value = Color::red; return value; }\n");
            scpp::monomorphize_generics(program);
            scpp::check_moves(program);
            scpp::Codegen codegen("test_module");
            codegen.generate(program);
        } catch (const scpp::DataflowError&) {
            threw = true;
        } catch (const scpp::CodegenError&) {
            threw = true;
        } catch (const scpp::ParseError&) {
            threw = true;
        }
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
        scpp::compile_to_executable(
            "import colors;\n"
            "int main() {\n"
            "    uint8_t lhs = static_cast<uint8_t>(colors::favorite());\n"
            "    uint8_t rhs = static_cast<uint8_t>(colors::Color::green);\n"
            "    return (int)lhs - (int)rhs;\n"
            "}\n",
            exe_path.string(), {}, {{"colors", module_path.string()}});
        RunResult run_result = run_command_capture(exe_path.string() + " 2>&1");
        expect(run_result.exit_code == 0,
               case_name + ": expected imported enum module executable to succeed, got " +
                   std::to_string(run_result.exit_code));
        std::filesystem::remove_all(root);
    }
}

void run_global_scope_resolution_tests() {
    {
        std::string case_name = "global_scope_resolution_bypasses_namespace_shadowing";
        cases_run++;
        std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
        scpp::compile_to_executable(
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
        RunResult result = run_command_capture(exe_path.string() + " 2>&1");
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
        std::filesystem::remove(exe_path);
    }
}

void run_expected_tests() {
    {
        std::string case_name = "std_expected_success_and_error_paths_work";
        cases_run++;
        RunResult result = compile_and_run(
            R"SCPP(import std;
enum class calc_error { invalid };
std::expected<int, calc_error> ok() {
    std::expected<int, calc_error> result(42);
    return std::move(result);
}
std::expected<int, calc_error> fail() {
    std::unexpected<calc_error> err(calc_error::invalid);
    std::expected<int, calc_error> result(err);
    return std::move(result);
}
int main() {
    std::expected<int, calc_error> good = ok();
    if (!good.has_value()) return 1;
    if (good.value() != 42) return 2;
    std::expected<int, calc_error> bad = fail();
    if (bad.has_value()) return 3;
    if (bad.error() != calc_error::invalid) return 4;
    return 0;
}
)SCPP",
            case_name);
        expect(result.exit_code == 0, case_name + ": expected exit code 0, got " + std::to_string(result.exit_code));
    }
}

} // namespace

int main() {
    run_test_case_files();
    run_error_location_tests();
    run_module_system_tests();
    run_concept_tests();
    run_generic_type_tests();
    run_generic_pack_deduction_tests();
    run_generic_function_overload_tests();
    run_functional_tests();
    run_thread_tests();
    run_global_scope_resolution_tests();
    run_expected_tests();
    run_enum_tests();
    test_compile_time_payload_plan_collects_exported_roots_and_helpers();
    run_sizeof_tests();
    run_storage_tests();
    run_placement_new_tests();
    run_explicit_destructor_tests();
    run_consteval_tests();
    run_cli_extension_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
