import scpp.driver;

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        failures++;
    }
}

// Compiles `source` to a temporary executable, runs it, and returns its exit
// code (as a plain 0-255 value, matching POSIX wait status semantics).
int compile_and_run(std::string_view source, const std::string& case_name) {
    std::filesystem::path exe_path = std::filesystem::temp_directory_path() / ("scpp_driver_test_" + case_name);
    scpp::compile_to_executable(source, exe_path.string());
    int status = std::system(exe_path.string().c_str());
    std::filesystem::remove(exe_path);
    return WEXITSTATUS(status);
}

void test_return_constant() {
    int exit_code = compile_and_run("int main() { return 42; }", "return_constant");
    expect(exit_code == 42, "return_constant: expected exit code 42");
}

void test_arithmetic() {
    int exit_code = compile_and_run("int main() { return 2 + 3 * 4; }", "arithmetic");
    expect(exit_code == 14, "arithmetic: expected exit code 14 (2 + 3*4)");
}

void test_if_else() {
    int exit_code = compile_and_run(
        "int f(int x) { if (x < 10) { return 1; } else { return 0; } }"
        "int main() { return f(5); }",
        "if_else");
    expect(exit_code == 1, "if_else: expected exit code 1");
}

void test_while_loop() {
    int exit_code = compile_and_run(
        "int main() { int x = 0; while (x < 10) { x = x + 1; } return x; }", "while_loop");
    expect(exit_code == 10, "while_loop: expected exit code 10");
}

void test_function_call() {
    int exit_code = compile_and_run(
        "int add(int a, int b) { return a + b; } int main() { return add(3, 4); }", "function_call");
    expect(exit_code == 7, "function_call: expected exit code 7");
}

void test_logical_short_circuit() {
    int exit_code = compile_and_run(
        "int main() { bool b = true || false; if (b) { return 1; } return 0; }", "logical_short_circuit");
    expect(exit_code == 1, "logical_short_circuit: expected exit code 1");
}

} // namespace

int main() {
    test_return_constant();
    test_arithmetic();
    test_if_else();
    test_while_loop();
    test_function_call();
    test_logical_short_circuit();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All driver tests passed.\n";
    return 0;
}
