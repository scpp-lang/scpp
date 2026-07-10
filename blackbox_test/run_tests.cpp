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
//    `COMPILE_ERROR` on its first line. The test passes if `scpp`
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
// Optional CLI-case sidecars:
//   - `<name>.argv` / `main.argv`: one argv token per non-blank line,
//     passed to `scpp` after placeholder substitution (`$INPUT`,
//     `$OUTPUT`, `$TEMP`).
//   - `<name>.mode` / `main.mode`: `command-only` means compare the CLI
//     command's own exit code/stdout instead of running a produced binary.
//   - `<name>.output` / `main.output`: output file path relative to the
//     case temp directory (default: `case.bin`); `*`/`**` globs are allowed
//     for paths such as target-triple-dependent build outputs.
//   - `<name>.artifacts` / `main.artifacts`: relative paths that must exist
//     after the CLI command succeeds; prefix a path with `!` to assert that
//     it was *not* produced.
//   - `<name>.stderr` / `main.stderr`: exact expected stderr from the CLI
//     command; `$TEMP` expands to the per-case temp directory so diagnostics
//     mentioning copied fixture paths can still be asserted exactly.
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
//     case directory and passed to `scpp` as
//     `--import module_name=path` (ch11 §11.14) -- list every module
//     `main.scpp` needs, direct or transitive (re-exported), since only
//     `main.scpp` itself is ever the compiled entry point.
//   - any other `.scpp` files in the directory -- the modules referenced
//     by `main.imports`; never scanned as their own standalone case.
//   - when `main.argv` is present, the whole case directory is copied into
//     the temp workspace before invoking `scpp`, so manifest/project-mode
//     cases can safely include `scpp.toml`, subpackages, and nested sources
//     without polluting the checked-in fixtures.
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
#ifndef SCPP_STDLIB_IO_WRAPPER_LIB_PATH
#error "SCPP_STDLIB_IO_WRAPPER_LIB_PATH must be defined by the build"
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
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

// Runs `argv` as a child process in `cwd`, redirecting its stdout/stderr to temp
// files under `temp_dir` -- reading them back only after the child exits
// avoids the pipe-buffer deadlock risk that concurrently reading two live
// pipes would carry -- and waits up to `timeout` before killing it.
RunResult run_process(const std::vector<std::string>& argv, const fs::path& temp_dir,
                       std::chrono::seconds timeout, const fs::path& cwd = fs::current_path()) {
    fs::path out_path = temp_dir / "stdout.txt";
    fs::path err_path = temp_dir / "stderr.txt";
    RunResult result;

    pid_t pid = fork();
    if (pid < 0) {
        result.err = "fork() failed";
        return result;
    }

    if (pid == 0) {
        if (chdir(cwd.c_str()) != 0) {
            std::perror("chdir");
            _exit(127);
        }
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

enum class RunnerMode { RunOutput, CommandOnly };

struct InvocationSpec {
    RunnerMode mode = RunnerMode::RunOutput;
    std::vector<std::string> argv_tokens;
    std::string output_relpath = "case.bin";
    std::vector<std::string> artifact_relpaths;
    std::optional<fs::path> stderr_expected_file;
};

std::vector<std::string> default_std_build_args() {
    return {"--import", std::string("std=") + SCPP_STDLIB_STD_MODULE_PATH,
            "--import", std::string("std:string=") + SCPP_STDLIB_STD_STRING_MODULE_PATH,
            "--import", std::string("std:memory=") + SCPP_STDLIB_STD_MEMORY_MODULE_PATH,
            "--import", std::string("std:functional=") + SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH,
            "--import", std::string("std:thread=") + SCPP_STDLIB_STD_THREAD_MODULE_PATH,
            "--link", SCPP_STDLIB_STRING_WRAPPER_LIB_PATH,
            "--link", SCPP_STDLIB_THREAD_WRAPPER_LIB_PATH,
            "--link", SCPP_STDLIB_IO_WRAPPER_LIB_PATH};
}

std::vector<std::string> parse_token_file(const fs::path& path) {
    std::vector<std::string> tokens;
    if (!fs::exists(path)) {
        return tokens;
    }

    std::istringstream stream(read_file(path));
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            continue;
        }
        line = line.substr(start);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        tokens.push_back(line);
    }
    return tokens;
}

std::string read_trimmed_file(const fs::path& path) {
    std::string text = read_file(path);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string replace_all(std::string text, std::string_view needle, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        text.replace(pos, needle.size(), replacement);
        pos += replacement.size();
    }
    return text;
}

bool contains_glob(std::string_view text) {
    return text.find('*') != std::string_view::npos || text.find('?') != std::string_view::npos;
}

std::regex glob_to_regex(std::string_view pattern) {
    std::string regex = "^";
    for (size_t i = 0; i < pattern.size(); i++) {
        char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                regex += ".*";
                i++;
            } else {
                regex += "[^/]*";
            }
            continue;
        }
        if (ch == '?') {
            regex += "[^/]";
            continue;
        }
        switch (ch) {
            case '.':
            case '^':
            case '$':
            case '+':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
            case '\\':
                regex.push_back('\\');
                break;
            default:
                break;
        }
        regex.push_back(ch);
    }
    regex += "$";
    return std::regex(regex);
}

std::vector<fs::path> match_relpaths(const fs::path& root, std::string_view relpath_pattern) {
    std::vector<fs::path> matches;
    if (!contains_glob(relpath_pattern)) {
        fs::path candidate = root / fs::path(relpath_pattern);
        if (fs::exists(candidate)) matches.push_back(candidate);
        return matches;
    }

    std::regex pattern = glob_to_regex(relpath_pattern);
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::string rel = fs::relative(entry.path(), root).generic_string();
        if (std::regex_match(rel, pattern)) matches.push_back(entry.path());
    }
    std::sort(matches.begin(), matches.end());
    return matches;
}

std::optional<std::string> resolve_single_path(const fs::path& root, std::string_view relpath_pattern,
                                               fs::path& resolved) {
    std::vector<fs::path> matches = match_relpaths(root, relpath_pattern);
    if (matches.empty()) {
        return "expected path '" + std::string(relpath_pattern) + "' was not produced";
    }
    if (matches.size() > 1) {
        return "path pattern '" + std::string(relpath_pattern) + "' matched multiple outputs";
    }
    resolved = matches.front();
    return std::nullopt;
}

std::vector<std::string> resolve_invocation_tokens(const std::vector<std::string>& raw_tokens, const fs::path& input_path,
                                                   const fs::path& temp_input_path, const fs::path& output_path,
                                                   const fs::path& temp_dir) {
    std::vector<std::string> resolved;
    resolved.reserve(raw_tokens.size());
    for (std::string token : raw_tokens) {
        token = replace_all(std::move(token), "$INPUT_FILE", temp_input_path.filename().string());
        token = replace_all(std::move(token), "$INPUT", input_path.string());
        token = replace_all(std::move(token), "$OUTPUT", output_path.string());
        token = replace_all(std::move(token), "$TEMP", temp_dir.string());
        resolved.push_back(std::move(token));
    }
    return resolved;
}

std::optional<std::string> compare_expected_stderr(const InvocationSpec& invocation, const std::string& actual_stderr,
                                                   const fs::path& temp_dir) {
    if (!invocation.stderr_expected_file) {
        return std::nullopt;
    }
    std::string expected_stderr = read_file(*invocation.stderr_expected_file);
    expected_stderr = replace_all(std::move(expected_stderr), "$TEMP", temp_dir.string());
    constexpr std::string_view kRegexPrefix = "REGEX:";
    if (expected_stderr.rfind(kRegexPrefix, 0) == 0) {
        while (!expected_stderr.empty() &&
               (expected_stderr.back() == '\n' || expected_stderr.back() == '\r')) {
            expected_stderr.pop_back();
        }
        try {
            std::regex pattern(expected_stderr.substr(kRegexPrefix.size()));
            if (std::regex_match(actual_stderr, pattern)) {
                return std::nullopt;
            }
        } catch (const std::regex_error& e) {
            return std::string("invalid stderr regex: ") + e.what();
        }
        return "expected stderr regex '" + expected_stderr.substr(kRegexPrefix.size()) + "', got '" + actual_stderr + "'";
    }
    if (actual_stderr == expected_stderr) {
        return std::nullopt;
    }
    return "expected stderr '" + expected_stderr + "', got '" + actual_stderr + "'";
}

std::optional<std::string> check_expected_artifacts(const InvocationSpec& invocation, const fs::path& temp_dir) {
    for (const std::string& raw_relpath : invocation.artifact_relpaths) {
        bool should_be_absent = !raw_relpath.empty() && raw_relpath[0] == '!';
        std::string relpath = should_be_absent ? raw_relpath.substr(1) : raw_relpath;
        std::vector<fs::path> matches = match_relpaths(temp_dir, relpath);
        if (should_be_absent) {
            if (!matches.empty()) {
                return "artifact '" + relpath + "' should not exist";
            }
            continue;
        }
        if (matches.empty()) {
            return "expected artifact '" + relpath + "' was not produced";
        }
        if (matches.size() > 1) {
            return "artifact pattern '" + relpath + "' matched multiple outputs";
        }
    }
    return std::nullopt;
}

void copy_tree_contents(const fs::path& from_dir, const fs::path& to_dir) {
    std::error_code ec;
    fs::create_directories(to_dir, ec);
    for (const auto& entry : fs::recursive_directory_iterator(from_dir)) {
        fs::path rel = fs::relative(entry.path(), from_dir);
        fs::path dest = to_dir / rel;
        if (entry.is_directory()) {
            fs::create_directories(dest, ec);
            continue;
        }
        fs::create_directories(dest.parent_path(), ec);
        fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
    }
}

bool path_is_within(const fs::path& path, const fs::path& ancestor) {
    fs::path normalized_path = path.lexically_normal();
    fs::path normalized_ancestor = ancestor.lexically_normal();
    auto path_it = normalized_path.begin();
    auto ancestor_it = normalized_ancestor.begin();
    for (; ancestor_it != normalized_ancestor.end(); ++ancestor_it, ++path_it) {
        if (path_it == normalized_path.end() || *path_it != *ancestor_it) return false;
    }
    return true;
}

// Parses a module test case directory's `main.imports`, if present: each
// non-blank, non-`#`-comment line is `module_name=relative_path`,
// resolved relative to `dir`, and turned into a `--import
// module_name=absolute_path` pair of arguments for `scpp` (ch11
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
                      const fs::path& temp_dir, const std::vector<std::string>& extra_build_args,
                      const InvocationSpec& invocation) {
    Expected expected = parse_expected(read_file(expected_path));
    fs::path case_temp_dir = temp_dir / "case";
    fs::path out_binary = case_temp_dir / invocation.output_relpath;
    fs::path temp_input_path = case_temp_dir / scpp_path.filename();
    fs::path compile_cwd = case_temp_dir;
    std::error_code ec;
    fs::remove_all(case_temp_dir, ec);
    fs::create_directories(case_temp_dir, ec);
    for (const std::string& relpath : invocation.artifact_relpaths) {
        std::string trimmed = (!relpath.empty() && relpath[0] == '!') ? relpath.substr(1) : relpath;
        fs::remove_all(case_temp_dir / trimmed, ec);
    }

    std::vector<std::string> build_argv;
    if (invocation.argv_tokens.empty()) {
        build_argv = {scpp_bin.string(), scpp_path.string(), "-o", out_binary.string()};
        std::vector<std::string> default_build_args = default_std_build_args();
        build_argv.insert(build_argv.end(), default_build_args.begin(), default_build_args.end());
        build_argv.insert(build_argv.end(), extra_build_args.begin(), extra_build_args.end());
    } else {
        if (scpp_path.filename() == "main.scpp") {
            copy_tree_contents(scpp_path.parent_path(), case_temp_dir);
            temp_input_path = case_temp_dir / "main.scpp";
        } else {
            fs::copy_file(scpp_path, temp_input_path, fs::copy_options::overwrite_existing, ec);
        }
        build_argv.push_back(scpp_bin.string());
        std::vector<std::string> resolved =
            resolve_invocation_tokens(invocation.argv_tokens, scpp_path, temp_input_path, out_binary, case_temp_dir);
        build_argv.insert(build_argv.end(), resolved.begin(), resolved.end());
    }
    RunResult compile_result =
        run_process(build_argv, case_temp_dir, std::chrono::seconds(kTimeoutSeconds), compile_cwd);

    if (compile_result.timed_out) {
        return {false, "scpp invocation timed out"};
    }

    if (expected.kind == ExpectedKind::CompileError) {
        if (compile_result.exited_normally && compile_result.exit_code > 0) {
            if (auto stderr_problem = compare_expected_stderr(invocation, compile_result.err, case_temp_dir)) {
                return {false, *stderr_problem};
            }
            return {true, ""};
        }
        if (compile_result.exited_normally && compile_result.exit_code == 0) {
            return {false, "expected a compile error, but `scpp` succeeded"};
        }
        return {false, "expected a clean compile error (positive exit code), but the compiler crashed; stderr:\n" +
                            compile_result.err};
    }

    if (invocation.mode == RunnerMode::CommandOnly) {
        if (!(compile_result.exited_normally && compile_result.exit_code == expected.exit_code)) {
            return {false, "expected command exit code " + std::to_string(expected.exit_code) + ", got " +
                                std::to_string(normalized_exit_code(compile_result))};
        }
        if (compile_result.out != expected.stdout_text) {
            return {false, "expected stdout '" + expected.stdout_text + "', got '" + compile_result.out + "'"};
        }
        if (auto stderr_problem = compare_expected_stderr(invocation, compile_result.err, case_temp_dir)) {
            return {false, *stderr_problem};
        }
        if (auto artifact_problem = check_expected_artifacts(invocation, case_temp_dir)) {
            return {false, *artifact_problem};
        }
        return {true, ""};
    }

    // ExpectedKind::Run or NoAbort: compilation itself must succeed.
    if (!(compile_result.exited_normally && compile_result.exit_code == 0)) {
        return {false, "expected successful compilation, but `scpp` failed; stderr:\n" + compile_result.err};
    }
    if (auto stderr_problem = compare_expected_stderr(invocation, compile_result.err, case_temp_dir)) {
        return {false, *stderr_problem};
    }
    if (auto artifact_problem = check_expected_artifacts(invocation, case_temp_dir)) {
        return {false, *artifact_problem};
    }
    if (auto output_problem = resolve_single_path(case_temp_dir, invocation.output_relpath, out_binary)) {
        return {false, *output_problem};
    }

    RunResult run_result =
        run_process({out_binary.string()}, case_temp_dir, std::chrono::seconds(kTimeoutSeconds), case_temp_dir);
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
    InvocationSpec invocation;
};

InvocationSpec load_invocation_spec(const fs::path& stem_path) {
    InvocationSpec spec;

    fs::path argv_path = stem_path;
    argv_path += ".argv";
    spec.argv_tokens = parse_token_file(argv_path);

    fs::path mode_path = stem_path;
    mode_path += ".mode";
    if (fs::exists(mode_path) && read_trimmed_file(mode_path) == "command-only") {
        spec.mode = RunnerMode::CommandOnly;
    }

    fs::path output_path = stem_path;
    output_path += ".output";
    if (fs::exists(output_path)) {
        spec.output_relpath = read_trimmed_file(output_path);
    }

    fs::path artifacts_path = stem_path;
    artifacts_path += ".artifacts";
    spec.artifact_relpaths = parse_token_file(artifacts_path);

    fs::path stderr_path = stem_path;
    stderr_path += ".stderr";
    if (fs::exists(stderr_path)) {
        spec.stderr_expected_file = stderr_path;
    }

    return spec;
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
        units.push_back(TestUnit{entry, expected, rel_name, parse_imports_file(dir), load_invocation_spec(dir / "main")});
    }

    // Pass 2: every other `.scpp` file is a standalone single-file case,
    // unless it lives inside a module-test directory already handled above
    // (its own main.scpp, or a helper module file main.scpp imports).
    for (const auto& entry : fs::recursive_directory_iterator(cases_dir)) {
        if (entry.path().extension() != ".scpp" || entry.path().filename() == "main.scpp") {
            continue;
        }
        bool inside_module_test_dir = std::any_of(module_test_dirs.begin(), module_test_dirs.end(),
                                                  [&](const fs::path& dir) { return path_is_within(entry.path(), dir); });
        if (inside_module_test_dir) {
            continue;
        }
        if (entry.path().string().find(filter) == std::string::npos) {
            continue;
        }
        fs::path expected_path = entry.path();
        expected_path.replace_extension(".expected");
        fs::path stem_path = entry.path();
        stem_path.replace_extension("");
        units.push_back(
            TestUnit{entry.path(), expected_path, fs::relative(entry.path(), cases_dir).string(), {}, load_invocation_spec(stem_path)});
    }
    std::sort(units.begin(), units.end(),
              [](const TestUnit& a, const TestUnit& b) { return a.rel_name < b.rel_name; });

    if (units.empty()) {
        std::cerr << "error: no case files found under " << cases_dir << " matching '" << filter << "'\n";
        return 2;
    }

    fs::path temp_dir = cases_dir.parent_path() / (".scpp_blackbox_test_" + std::to_string(getpid()));
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

        Outcome outcome = run_one_case(*scpp_bin, unit.entry_file, unit.expected_file, temp_dir, unit.extra_build_args,
                                       unit.invocation);
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
