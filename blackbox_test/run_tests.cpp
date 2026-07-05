// Black-box test runner for the scpp compiler.
//
// This runner treats `scpp` purely as an external CLI tool -- exactly the
// way a user following docs/book would -- and never links against any of
// scpp's internal compiler modules. It is a plain, dependency-free C++
// program (POSIX fork/exec + <filesystem>, nothing else) so running the
// suite never requires anything beyond a C++ compiler.
//
// For each `<name>.cpp` file under `cases/`, it looks for a sibling
// `<name>.expected` file describing the required outcome, in one of three
// forms:
//
// 1. Successful compile + run: the first line is the expected process
//    exit code (0-255, matching POSIX WEXITSTATUS/shell `$?` semantics; a
//    process killed by a signal is normalized to 128+signum, e.g. SIGABRT
//    -> 134, matching this repo's existing tests/test_source convention).
//    Everything after the first newline is the expected stdout, compared
//    byte-for-byte.
// 2. Compile-time rejection: the file contains exactly the sentinel
//    `COMPILE_ERROR` on its first line. The test passes if `scpp build`
//    exits with a positive (non-signal) status -- i.e. a clean, controlled
//    error, not a crash -- regardless of the exact diagnostic wording (the
//    spec does not pin down message text).
// 3. Compile + run, but the exact value is unspecified: the file contains
//    exactly the sentinel `NO_ABORT` on its first line. Used for the
//    handful of cases where a scpp-inserted runtime check (span bounds,
//    overflow) is *skipped* (inside unsafe { }/a native function per ch01
//    §1.3), so the read/computed value is genuine, unspecified garbage --
//    not something a test can pin down -- but the process must still
//    terminate normally (return/exit) rather than being killed by a
//    signal.
//
// Adding a new case is just dropping in a matching `.cpp`/`.expected` pair
// under `cases/` (subdirectories are just organization and are walked
// recursively) -- no changes to this file are needed.
//
// Build: cmake -S . -B build && cmake --build build (see CMakeLists.txt).
// Usage: ./build/run_tests [filter] [--scpp-bin <path>]

// SCPP_BLACKBOX_TEST_CASES_DIR / SCPP_DEFAULT_SCPP_BINARY are injected
// by CMake (see CMakeLists.txt) as absolute paths computed at configure
// time, so this binary finds its fixtures and a default `scpp` build
// regardless of the working directory it's run from, or where the build
// system places the compiled executable -- the same pattern already used
// by tests/driver_test.cpp et al. one directory up.
#ifndef SCPP_BLACKBOX_TEST_CASES_DIR
#error "SCPP_BLACKBOX_TEST_CASES_DIR must be defined by the build"
#endif
#ifndef SCPP_DEFAULT_SCPP_BINARY
#error "SCPP_DEFAULT_SCPP_BINARY must be defined by the build"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kCompileErrorSentinel = "COMPILE_ERROR";
constexpr const char* kNoAbortSentinel = "NO_ABORT";
constexpr int kTimeoutSeconds = 15;

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Raw outcome of running one child process.
struct RunResult {
    bool timed_out = false;
    bool exited_normally = false; // true if the process returned from main()/called exit()
    int exit_code = 0;            // valid only if exited_normally
    int signal_number = 0;        // valid only if !exited_normally && !timed_out
    std::string out;
    std::string err;
};

// Runs `argv` as a child process, redirecting its stdout/stderr to temp
// files under `temp_dir` -- reading them back only after the child exits
// avoids the pipe-buffer deadlock risk that concurrently reading two live
// pipes would carry -- and waits up to `timeout` before killing it.
RunResult run_process(const std::vector<std::string>& argv, const fs::path& temp_dir,
                       std::chrono::seconds timeout) {
    fs::path out_path = temp_dir / "stdout.txt";
    fs::path err_path = temp_dir / "stderr.txt";
    RunResult result;

    pid_t pid = fork();
    if (pid < 0) {
        result.err = "fork() failed";
        return result;
    }

    if (pid == 0) {
        // Child: redirect stdout/stderr to the temp files, then exec.
        int out_fd = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int err_fd = open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out_fd >= 0) {
            dup2(out_fd, STDOUT_FILENO);
        }
        if (err_fd >= 0) {
            dup2(err_fd, STDERR_FILENO);
        }

        std::vector<char*> c_argv;
        c_argv.reserve(argv.size() + 1);
        for (const std::string& a : argv) {
            c_argv.push_back(const_cast<char*>(a.c_str()));
        }
        c_argv.push_back(nullptr);

        execvp(c_argv[0], c_argv.data());
        std::perror("execvp"); // only reached if exec itself failed
        _exit(127);
    }

    // Parent: poll for completion with WNOHANG so a hung child can't block
    // the whole suite past `timeout`.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    int status = 0;
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!finished) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        result.timed_out = true;
    } else if (WIFEXITED(status)) {
        result.exited_normally = true;
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exited_normally = false;
        result.signal_number = WTERMSIG(status);
    }

    result.out = read_file(out_path);
    result.err = read_file(err_path);
    std::error_code ec;
    fs::remove(out_path, ec);
    fs::remove(err_path, ec);
    return result;
}

// Mirrors POSIX WEXITSTATUS/shell `$?` semantics for a signal-terminated
// process (e.g. 134 for SIGABRT), matching this repo's existing
// tests/test_source convention.
int normalized_exit_code(const RunResult& r) {
    return r.exited_normally ? r.exit_code : 128 + r.signal_number;
}

enum class ExpectedKind { Run, CompileError, NoAbort };

struct Expected {
    ExpectedKind kind = ExpectedKind::Run;
    int exit_code = 0;
    std::string stdout_text;
};

Expected parse_expected(const std::string& text) {
    size_t newline = text.find('\n');
    std::string first_line = newline == std::string::npos ? text : text.substr(0, newline);
    std::string rest = newline == std::string::npos ? "" : text.substr(newline + 1);
    while (!first_line.empty() && (first_line.back() == '\r' || first_line.back() == ' ')) {
        first_line.pop_back();
    }

    Expected expected;
    if (first_line == kCompileErrorSentinel) {
        expected.kind = ExpectedKind::CompileError;
    } else if (first_line == kNoAbortSentinel) {
        expected.kind = ExpectedKind::NoAbort;
    } else {
        expected.kind = ExpectedKind::Run;
        expected.exit_code = std::stoi(first_line);
        expected.stdout_text = rest;
    }
    return expected;
}

struct Outcome {
    bool passed = false;
    std::string detail;
};

Outcome run_one_case(const fs::path& scpp_bin, const fs::path& cpp_path, const fs::path& expected_path,
                      const fs::path& temp_dir) {
    Expected expected = parse_expected(read_file(expected_path));
    fs::path out_binary = temp_dir / "case.bin";
    std::error_code ec;
    fs::remove(out_binary, ec);

    RunResult compile_result = run_process({scpp_bin.string(), "build", cpp_path.string(), "-o", out_binary.string()},
                                            temp_dir, std::chrono::seconds(kTimeoutSeconds));

    if (compile_result.timed_out) {
        return {false, "scpp build timed out"};
    }

    if (expected.kind == ExpectedKind::CompileError) {
        if (compile_result.exited_normally && compile_result.exit_code > 0) {
            return {true, ""};
        }
        if (compile_result.exited_normally && compile_result.exit_code == 0) {
            return {false, "expected a compile error, but `scpp build` succeeded"};
        }
        return {false, "expected a clean compile error (positive exit code), but the compiler crashed; stderr:\n" +
                            compile_result.err};
    }

    // ExpectedKind::Run or NoAbort: compilation itself must succeed.
    if (!(compile_result.exited_normally && compile_result.exit_code == 0)) {
        return {false, "expected successful compilation, but `scpp build` failed; stderr:\n" + compile_result.err};
    }

    RunResult run_result = run_process({out_binary.string()}, temp_dir, std::chrono::seconds(kTimeoutSeconds));
    fs::remove(out_binary, ec);

    if (run_result.timed_out) {
        return {false, "compiled binary timed out while running"};
    }

    if (expected.kind == ExpectedKind::NoAbort) {
        if (!run_result.exited_normally) {
            return {false, "expected the process to terminate normally (no scpp-inserted check should fire here), "
                           "but it was killed by signal " +
                               std::to_string(run_result.signal_number)};
        }
        return {true, ""};
    }

    int actual_exit = normalized_exit_code(run_result);
    std::vector<std::string> problems;
    if (actual_exit != expected.exit_code) {
        problems.push_back("expected exit code " + std::to_string(expected.exit_code) + ", got " +
                            std::to_string(actual_exit));
    }
    if (run_result.out != expected.stdout_text) {
        problems.push_back("expected stdout '" + expected.stdout_text + "', got '" + run_result.out + "'");
    }
    if (!run_result.err.empty()) {
        problems.push_back("unexpected stderr: '" + run_result.err + "'");
    }
    if (problems.empty()) {
        return {true, ""};
    }
    std::string joined;
    for (size_t i = 0; i < problems.size(); i++) {
        if (i > 0) {
            joined += "; ";
        }
        joined += problems[i];
    }
    return {false, joined};
}

std::optional<fs::path> find_default_scpp_binary() {
    // The configure-time default (../build/scpp relative to this project)
    // covers the ordinary layout; fall back to a couple of cwd-relative
    // guesses in case scpp was built somewhere nonstandard.
    std::vector<fs::path> candidates = {
        fs::path(SCPP_DEFAULT_SCPP_BINARY),
        fs::current_path() / ".." / "build" / "scpp",
        fs::current_path() / "build" / "scpp",
    };
    for (const fs::path& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            fs::path canonical = fs::canonical(candidate, ec);
            return ec ? candidate : canonical;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<fs::path> scpp_bin;
    std::string filter;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--scpp-bin" && i + 1 < argc) {
            scpp_bin = fs::path(argv[++i]);
        } else {
            filter = arg;
        }
    }
    if (!scpp_bin) {
        scpp_bin = find_default_scpp_binary();
    }
    if (!scpp_bin || !fs::exists(*scpp_bin)) {
        std::cerr << "error: could not find the scpp binary. Build it first (see ../README.md), or pass "
                     "--scpp-bin <path>.\n";
        return 2;
    }

    fs::path cases_dir(SCPP_BLACKBOX_TEST_CASES_DIR);
    if (!fs::exists(cases_dir)) {
        std::cerr << "error: could not find the cases/ directory (looked at " << cases_dir << ")\n";
        return 2;
    }

    std::vector<fs::path> cpp_files;
    for (const auto& entry : fs::recursive_directory_iterator(cases_dir)) {
        if (entry.path().extension() == ".cpp" && entry.path().string().find(filter) != std::string::npos) {
            cpp_files.push_back(entry.path());
        }
    }
    std::sort(cpp_files.begin(), cpp_files.end());

    if (cpp_files.empty()) {
        std::cerr << "error: no *.cpp case files found under " << cases_dir << " matching '" << filter << "'\n";
        return 2;
    }

    fs::path temp_dir = fs::temp_directory_path() / ("scpp_blackbox_test_" + std::to_string(getpid()));
    fs::create_directories(temp_dir);
    std::error_code ec;

    int passed = 0;
    std::vector<std::pair<std::string, std::string>> failed;

    for (const fs::path& cpp_path : cpp_files) {
        fs::path expected_path = cpp_path;
        expected_path.replace_extension(".expected");
        std::string rel_name = fs::relative(cpp_path, cases_dir).string();

        if (!fs::exists(expected_path)) {
            failed.emplace_back(rel_name, "missing " + expected_path.filename().string());
            std::cout << "FAIL " << rel_name << ": missing " << expected_path.filename().string() << "\n";
            continue;
        }

        Outcome outcome = run_one_case(*scpp_bin, cpp_path, expected_path, temp_dir);
        if (outcome.passed) {
            passed++;
            std::cout << "ok   " << rel_name << "\n";
        } else {
            failed.emplace_back(rel_name, outcome.detail);
            std::cout << "FAIL " << rel_name << ": " << outcome.detail << "\n";
        }
    }

    fs::remove_all(temp_dir, ec);

    int total = passed + static_cast<int>(failed.size());
    std::cout << "\n" << passed << "/" << total << " case(s) passed.\n";
    if (!failed.empty()) {
        std::cout << "\n" << failed.size() << " failure(s):\n";
        for (const auto& [name, detail] : failed) {
            std::cout << "  - " << name << ": " << detail << "\n";
        }
        return 1;
    }
    return 0;
}
