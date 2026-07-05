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

struct RunResult {
    int exit_code;
    std::string stdout_text;
};

// Compiles `source` to a temporary executable, runs it, and captures both
// its stdout and exit code (0-255, matching POSIX wait status semantics).
RunResult compile_and_run(std::string_view source, const std::string& case_name) {
    std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
    scpp::compile_to_executable(source, exe_path.string());

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

// Runs every `<name>.cpp` case file under SCPP_TEST_SOURCE_DIR against its
// paired `<name>.expected` file (see parse_expected). Adding a new test case
// is just dropping in 2 new files -- no changes to this file or a rebuild of
// the test harness are needed, just re-running the already-built binary.
void run_test_case_files() {
    std::filesystem::path dir(SCPP_TEST_SOURCE_DIR);
    std::vector<std::filesystem::path> source_files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".cpp") {
            source_files.push_back(entry.path());
        }
    }
    std::sort(source_files.begin(), source_files.end());

    expect(!source_files.empty(), "expected at least one *.cpp test case in " + dir.string());

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
         "safe int f() {\n    std::unique_ptr<int> p = std::make_unique<int>(5);\n    std::unique_ptr<int> q = "
         "std::move(p);\n    return *p;\n}\nint main() { return f(); }\n",
         4},
    };
    for (const Case& c : dataflow_cases) {
        cases_run++;
        try {
            scpp::Program program = scpp::parse(c.source);
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
            std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name + "_" + suffix + ".cpp");
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
            "    export safe int square(int x) { return x * x; }\n"
            "    safe int helper(int x) { return x + 1; }\n"
            "    export safe int square_plus_one(int x) { return mathlib::square(x) + mathlib::helper(0); }\n"
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
            "    safe int helper(int x) { return x + 1; }\n"
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
}

} // namespace

int main() {
    run_test_case_files();
    run_error_location_tests();
    run_module_system_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
