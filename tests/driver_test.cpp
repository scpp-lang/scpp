import scpp.driver;
import scpp.parser;
import scpp.movecheck;
import scpp.codegen;
import scpp.ast;

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
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
#ifndef SCPP_STDLIB_STD_STRING_MODULE_PATH
#error "SCPP_STDLIB_STD_STRING_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_MEMORY_MODULE_PATH
#error "SCPP_STDLIB_STD_MEMORY_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH
#error "SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STRING_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_STRING_WRAPPER_LIB_PATH must be defined by the build"
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

std::unordered_map<std::string, std::string> std_import_paths() {
    return {{"std", SCPP_STDLIB_STD_MODULE_PATH},
            {"std:string", SCPP_STDLIB_STD_STRING_MODULE_PATH},
            {"std:memory", SCPP_STDLIB_STD_MEMORY_MODULE_PATH},
            {"std:functional", SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH}};
}

std::vector<std::string> std_link_inputs() {
    return {SCPP_STDLIB_STRING_WRAPPER_LIB_PATH};
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
        auto path_it = import_paths_.find(key);
        if (path_it == import_paths_.end()) throw std::runtime_error("unknown test partition '" + key + "'");
        return scpp::parse(
            read_file(path_it->second), [this](const std::string& name) -> const scpp::Program& { return resolve(name); },
            [this](const std::string& nested_key) -> scpp::Program { return resolve_partition(nested_key); });
    }

private:
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

void write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream file(path);
    file << content;
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

void run_cli_extension_tests() {
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
            run_command_capture(std::string(SCPP_BINARY_PATH) + " build " + main_path.string() + " -o " +
                                exe_path.string() + " --import helper=" + module_path.string() + " 2>&1");
        std::filesystem::remove(main_path);
        std::filesystem::remove(module_path);
        std::filesystem::remove(exe_path);
        expect(result.exit_code != 0, case_name + ": expected non-zero exit");
        expect(result.stdout_text.find("import path for module 'helper' must use the .scpp extension") !=
                   std::string::npos,
               case_name + ": expected import extension error, got '" + result.stdout_text + "'");
    }
}

} // namespace

int main() {
    run_test_case_files();
    run_error_location_tests();
    run_module_system_tests();
    run_concept_tests();
    run_generic_type_tests();
    run_functional_tests();
    run_cli_extension_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
