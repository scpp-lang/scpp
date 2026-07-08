// Black-box test runner for the scpp compiler.
//
// This runner treats `scpp` purely as an external CLI tool -- exactly the
// way a user following docs/book would -- and never links against any of
// scpp's internal compiler modules. It is a plain, dependency-free C++
// program (POSIX fork/exec + <filesystem>, nothing else) so running the
// suite never requires anything beyond a C++ compiler.
//
// For each `<name>.scpp` file under `cases/`, it looks for a sibling
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
//    overflow) is *skipped* inside an `unsafe { }` block (ch01 §1.1), so
//    the read/computed value is genuine, unspecified garbage --
//    not something a test can pin down -- but the process must still
//    terminate normally (return/exit) rather than being killed by a
//    signal.
//
// Adding a new single-file case is just dropping in a matching
// `.scpp`/`.expected` pair under `cases/` (subdirectories are just
// organization and are walked recursively) -- no changes to this file are
// needed.
//
// Multi-file (ch11 module) cases: some language rules (import/export
// across files, partitions, ...) genuinely need more than one source
// file. A directory containing a `main.scpp` file is instead treated as
// one *module test case*, named after the directory:
//   - `main.scpp` -- the entry point, compiled and run exactly like an
//     ordinary single-file case (`main.expected` is its outcome, same
//     three forms as above).
//   - `main.imports` (optional) -- one `module_name=relative_path` mapping
//     per non-blank, non-`#`-comment line, resolved relative to the test
//     case directory and passed to `scpp build` as
//     `--import module_name=path` (ch11 §11.14) -- list every module
//     `main.scpp` needs, direct or transitive (re-exported), since only
//     `main.scpp` itself is ever the compiled entry point.
//   - any other `.scpp` files in the directory -- the modules referenced
//     by `main.imports`; never scanned as their own standalone case.
// **Verified**: `--import name=path`'s `path` does point directly at that
// module's raw `.scpp` interface source, compiled on the fly -- there is
// no separate "compile a module to `.scppm` first" step. Confirmed
// empirically by several passing multi-file cases (see README.md's
// Status section for details).
// **Discovered constraint**: a file containing `export module name;` does
// not get its `main()` linked as the process entry point (an "undefined
// reference to `main`" linker error results if you try). Every
// module test case therefore needs its `main.scpp` to be a plain,
// non-moduled file that imports and calls into separate module file(s) --
// never a single file wearing both hats.
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
#ifndef SCPP_STDLIB_STD_THREAD_MODULE_PATH
#error "SCPP_STDLIB_STD_THREAD_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STRING_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_STRING_WRAPPER_LIB_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH must be defined by the build"
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

std::vector<std::string> default_std_build_args() {
    return {"--import", std::string("std=") + SCPP_STDLIB_STD_MODULE_PATH,
            "--import", std::string("std:string=") + SCPP_STDLIB_STD_STRING_MODULE_PATH,
            "--import", std::string("std:memory=") + SCPP_STDLIB_STD_MEMORY_MODULE_PATH,
            "--import", std::string("std:functional=") + SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH,
            "--import", std::string("std:thread=") + SCPP_STDLIB_STD_THREAD_MODULE_PATH,
            "--link", SCPP_STDLIB_STRING_WRAPPER_LIB_PATH,
            "--link", SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH};
}

// Parses a module test case directory's `main.imports`, if present: each
// non-blank, non-`#`-comment line is `module_name=relative_path`,
// resolved relative to `dir`, and turned into a `--import
// module_name=absolute_path` pair of arguments for `scpp build` (ch11
// §11.14).
std::vector<std::string> parse_imports_file(const fs::path& dir) {
    std::vector<std::string> args;
    fs::path imports_path = dir / "main.imports";
    if (!fs::exists(imports_path)) {
        return args;
    }

    std::istringstream stream(read_file(imports_path));
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            continue; // blank line
        }
        line = line.substr(start);
        if (line.empty() || line[0] == '#') {
            continue; // comment
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue; // malformed; skip rather than hard-fail the whole suite
        }
        std::string module_name = line.substr(0, eq);
        std::string rel_path = line.substr(eq + 1);
        fs::path abs_path = fs::absolute(dir / rel_path);
        args.push_back("--import");
        args.push_back(module_name + "=" + abs_path.string());
    }
    return args;
}

Outcome run_one_case(const fs::path& scpp_bin, const fs::path& scpp_path, const fs::path& expected_path,
                      const fs::path& temp_dir, const std::vector<std::string>& extra_build_args) {
    Expected expected = parse_expected(read_file(expected_path));
    fs::path out_binary = temp_dir / "case.bin";
    std::error_code ec;
    fs::remove(out_binary, ec);

    std::vector<std::string> build_argv = {scpp_bin.string(), "build", scpp_path.string(), "-o",
                                           out_binary.string()};
    std::vector<std::string> default_build_args = default_std_build_args();
    build_argv.insert(build_argv.end(), default_build_args.begin(), default_build_args.end());
    build_argv.insert(build_argv.end(), extra_build_args.begin(), extra_build_args.end());
    RunResult compile_result = run_process(build_argv, temp_dir, std::chrono::seconds(kTimeoutSeconds));

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

// One discovered test case, either a single loose `.scpp` file or a
// `main.scpp`-containing module-test directory (see this file's header
// comment).
struct TestUnit {
    fs::path entry_file;
    fs::path expected_file;
    std::string rel_name;
    std::vector<std::string> extra_build_args;
};

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

    // Pass 1: any directory containing a `main.scpp` is one module test
    // case (possibly multi-file); record its directory so pass 2 can skip
    // treating its files as standalone single-file cases.
    std::vector<fs::path> module_test_dirs;
    for (const auto& entry : fs::recursive_directory_iterator(cases_dir)) {
        if (entry.path().filename() == "main.scpp") {
            module_test_dirs.push_back(entry.path().parent_path());
        }
    }

    std::vector<TestUnit> units;
    for (const fs::path& dir : module_test_dirs) {
        fs::path entry = dir / "main.scpp";
        fs::path expected = dir / "main.expected";
        std::string rel_name = fs::relative(dir, cases_dir).string();
        if (rel_name.find(filter) == std::string::npos) {
            continue;
        }
        units.push_back(TestUnit{entry, expected, rel_name, parse_imports_file(dir)});
    }

    // Pass 2: every other `.scpp` file is a standalone single-file case,
    // unless it lives inside a module-test directory already handled above
    // (its own main.scpp, or a helper module file main.scpp imports).
    for (const auto& entry : fs::recursive_directory_iterator(cases_dir)) {
        if (entry.path().extension() != ".scpp" || entry.path().filename() == "main.scpp") {
            continue;
        }
        fs::path parent = entry.path().parent_path();
        bool inside_module_test_dir =
            std::find(module_test_dirs.begin(), module_test_dirs.end(), parent) != module_test_dirs.end();
        if (inside_module_test_dir) {
            continue;
        }
        if (entry.path().string().find(filter) == std::string::npos) {
            continue;
        }
        fs::path expected_path = entry.path();
        expected_path.replace_extension(".expected");
        units.push_back(TestUnit{entry.path(), expected_path, fs::relative(entry.path(), cases_dir).string(), {}});
    }
    std::sort(units.begin(), units.end(),
              [](const TestUnit& a, const TestUnit& b) { return a.rel_name < b.rel_name; });

    if (units.empty()) {
        std::cerr << "error: no case files found under " << cases_dir << " matching '" << filter << "'\n";
        return 2;
    }

    fs::path temp_dir = fs::temp_directory_path() / ("scpp_blackbox_test_" + std::to_string(getpid()));
    fs::create_directories(temp_dir);
    std::error_code ec;

    int passed = 0;
    std::vector<std::pair<std::string, std::string>> failed;

    for (const TestUnit& unit : units) {
        if (!fs::exists(unit.expected_file)) {
            failed.emplace_back(unit.rel_name, "missing " + unit.expected_file.filename().string());
            std::cout << "FAIL " << unit.rel_name << ": missing " << unit.expected_file.filename().string() << "\n";
            continue;
        }

        Outcome outcome =
            run_one_case(*scpp_bin, unit.entry_file, unit.expected_file, temp_dir, unit.extra_build_args);
        if (outcome.passed) {
            passed++;
            std::cout << "ok   " << unit.rel_name << "\n";
        } else {
            failed.emplace_back(unit.rel_name, outcome.detail);
            std::cout << "FAIL " << unit.rel_name << ": " << outcome.detail << "\n";
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
