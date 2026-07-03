import scpp.driver;

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

} // namespace

int main() {
    run_test_case_files();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
