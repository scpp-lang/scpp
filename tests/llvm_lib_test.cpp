// End-to-end proof that libs/scpp/llvm/ is independently usable by an
// ordinary scpp program, exactly as a real scpp user would use it: shells
// out to the real `scpp` compiler binary to build
// tests/llvm_lib_test_source/main.scpp against `import scpp;`, passing
// the same `--import`/`--link` flags any consumer of this package would
// need today (see libs/scpp/scpp.toml's own comment on why those `--link`
// flags aren't yet fully automatic), then runs the resulting binary and
// checks its stdout. This is deliberately a separate binary from
// driver_test (which exercises scpp::compile_to_executable() and the
// std/scpp libraries in-process): unlike those, this test's whole point
// is to prove the *real* CLI-driven, --import/--link-based workflow an
// external scpp consumer would follow actually works, not just that the
// C++ driver API does.
//
// This imports the *prebuilt* scpp.scppm interface (SCPP_STDLIB_SCPP_INTERFACE_PATH,
// the same one driver_test's prebuilt_module_import_paths() uses) rather
// than raw scpp.scpp source: the scpp compiler's own import resolution
// auto-discovers and links the co-located libscpp.scppa archive for any
// prebuilt-interface import (see driver.cppm's Driver::archive_for()),
// and that archive already folds in llvm/native_target_init.cpp's
// compiled object code (libs/scpp/scpp.toml's `[additional_objs.
// scpp-native]` step) -- so, unlike the official LLVM libraries
// themselves, this program needs no separate --link for that shim.
//
// `popen`/`pclose`/`FILE*` are POSIX extensions, not part of ISO C++'s
// standard library (they have no `import std;` module form), so `<cstdio>`
// stays as a real #include; same for `<sys/wait.h>` (POSIX `WIFEXITED` et
// al., used below).
#include <cstdio>
#include <sys/wait.h>

import std;

#ifndef SCPP_BINARY_PATH
#error "SCPP_BINARY_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_STD_MODULE_PATH
#error "SCPP_STDLIB_STD_MODULE_PATH must be defined by the build"
#endif
#ifndef SCPP_STDLIB_SCPP_INTERFACE_PATH
#error "SCPP_STDLIB_SCPP_INTERFACE_PATH must be defined by the build"
#endif
#ifndef SCPP_LLVM_NATIVE_LIBRARY_FILES
#error "SCPP_LLVM_NATIVE_LIBRARY_FILES must be defined by the build"
#endif
#ifndef SCPP_LLVM_LIB_TEST_SOURCE_DIR
#error "SCPP_LLVM_LIB_TEST_SOURCE_DIR must be defined by the build"
#endif
#ifndef SCPP_LLVM_LIB_TEST_WORK_DIR
#error "SCPP_LLVM_LIB_TEST_WORK_DIR must be defined by the build"
#endif

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        ++failures;
    }
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

// SCPP_LLVM_NATIVE_LIBRARY_FILES is one whitespace-separated string of
// absolute paths (llvm-config's own `--libfiles` output convention, see the
// top-level CMakeLists.txt); every real scpp:llvm consumer needs each of
// these as its own `--link` flag, since libs/scpp/llvm's scpp bindings
// call official LLVM-C functions directly (there is no wrapper archive of
// our own to link in addition) -- this project's own manifest mechanism
// has no way to declare "consumers also need external system library X at
// final link time" yet (see libs/scpp/scpp.toml's own comment), so real
// LLVM's own libraries stay an explicit `--link` a consumer supplies by
// hand. The one confirmed gap native_target_init.cpp bridges (see its own
// top-of-file comment) needs no such treatment here: importing the
// *prebuilt* scpp.scppm interface below (rather than raw source) makes
// the scpp compiler auto-discover and link the co-located libscpp.scppa
// archive, which already has that shim's compiled object code folded in
// (libs/scpp/scpp.toml's `[additional_objs.scpp-native]` step).
std::vector<std::string> split_whitespace(const std::string& text) {
    std::vector<std::string> parts;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        parts.push_back(token);
    }
    return parts;
}

std::string build_link_flags() {
    std::string flags;
    for (const std::string& lib : split_whitespace(SCPP_LLVM_NATIVE_LIBRARY_FILES)) {
        flags += " --link " + lib;
    }
    return flags;
}

void run_end_to_end_test() {
    std::filesystem::create_directories(SCPP_LLVM_LIB_TEST_WORK_DIR);
    std::filesystem::path source = std::filesystem::path(SCPP_LLVM_LIB_TEST_SOURCE_DIR) / "main.scpp";
    std::filesystem::path binary = std::filesystem::path(SCPP_LLVM_LIB_TEST_WORK_DIR) / "llvm_lib_test_bin";

    std::string build_command = std::string(SCPP_BINARY_PATH) + " " + source.string() + " -o " + binary.string() +
                                " --import std=" + SCPP_STDLIB_STD_MODULE_PATH + " --import scpp=" +
                                SCPP_STDLIB_SCPP_INTERFACE_PATH + build_link_flags() + " 2>&1";
    RunResult build_result = run_command_capture(build_command);
    expect(build_result.exit_code == 0,
           "libs/scpp/llvm/ example program compiles, got exit " + std::to_string(build_result.exit_code) + ": '" +
               build_result.stdout_text + "'");
    if (build_result.exit_code != 0) return;

    RunResult run_result = run_command_capture(binary.string() + " 2>&1");
    expect(run_result.exit_code == 0,
           "libs/scpp/llvm/ example program runs to completion, got exit " + std::to_string(run_result.exit_code) +
               ": '" + run_result.stdout_text + "'");

    const std::string& output = run_result.stdout_text;
    // llvm::DataLayout query results, x86-64: i32 is 4 bytes/32 bits and
    // naturally aligned to 4 bytes; i64 is 8 bytes; a pointer is 8 bytes,
    // naturally aligned to 8 bytes.
    expect(output.find("i32 alloc size: 4") != std::string::npos, "reports i32 alloc size 4, got '" + output + "'");
    expect(output.find("i32 size in bits: 32") != std::string::npos,
           "reports i32 size in bits 32, got '" + output + "'");
    expect(output.find("i32 abi align: 4") != std::string::npos, "reports i32 abi align 4, got '" + output + "'");
    expect(output.find("i64 alloc size: 8") != std::string::npos, "reports i64 alloc size 8, got '" + output + "'");
    expect(output.find("pointer size: 8") != std::string::npos, "reports pointer size 8, got '" + output + "'");
    // Module::pointer_abi_alignment composes two official LLVM-C calls
    // (llvm_core.scpp's own top comment has the full rationale); asserting
    // on it here proves that composition works end to end via the real
    // scpp compiler, not just the single-call direct-to-LLVM-C queries
    // above.
    expect(output.find("pointer abi align: 8") != std::string::npos,
           "reports pointer abi align 8, got '" + output + "'");

    // The remaining scalar Type factories (i1/i8/i16/float/double/void).
    expect(output.find("i1 size in bits: 1") != std::string::npos, "reports i1 size in bits 1, got '" + output + "'");
    expect(output.find("i8 size in bits: 8") != std::string::npos, "reports i8 size in bits 8, got '" + output + "'");
    expect(output.find("i16 size in bits: 16") != std::string::npos,
           "reports i16 size in bits 16, got '" + output + "'");
    expect(output.find("f32 size in bits: 32") != std::string::npos,
           "reports f32 size in bits 32, got '" + output + "'");
    expect(output.find("f64 size in bits: 64") != std::string::npos,
           "reports f64 size in bits 64, got '" + output + "'");

    // Derived Types: a 10-element i32 array is 40 bytes; the anonymous
    // {i32, f64, ptr} struct is 24 bytes (4-byte i32, 4 bytes of padding
    // up to f64's 8-byte alignment, 8-byte f64, 8-byte pointer). Both the
    // struct and the (i32, i64) -> i32 function type are also printed as
    // IR text (Type::print) to directly confirm every field/parameter
    // Type::get_struct/get_function's malloc/free marshalling handles
    // survived, in order -- not just an aggregate size/count.
    expect(output.find("array alloc size: 40") != std::string::npos,
           "reports array alloc size 40, got '" + output + "'");
    expect(output.find("struct alloc size: 24") != std::string::npos,
           "reports struct alloc size 24, got '" + output + "'");
    expect(output.find("{ i32, double, ptr }") != std::string::npos,
           "prints the anonymous struct's 3 field types in order, got '" + output + "'");
    expect(output.find("i32 (i32, i64)") != std::string::npos,
           "prints the function type's return and both parameter types, got '" + output + "'");

    // Simple Constants, printed as LLVM IR text via Value::print.
    expect(output.find("i32 42") != std::string::npos, "prints the i32 42 constant, got '" + output + "'");
    expect(output.find("double 3.500000e+00") != std::string::npos,
           "prints the double 3.5 constant, got '" + output + "'");
    expect(output.find("ptr null") != std::string::npos, "prints the null pointer constant, got '" + output + "'");
    expect(output.find("const_i32 type size in bits: 32") != std::string::npos,
           "Value::type_of round-trips an i32 constant's Type, got '" + output + "'");

    expect(output.find("module verified ok") != std::string::npos,
           "llvm::verifyModule succeeds on the freshly built module, got '" + output + "'");
    expect(output.find("ModuleID = 'llvm_lib_demo'") != std::string::npos,
           "prints valid LLVM IR text for the module, got '" + output + "'");
}

} // namespace

int main() {
    run_end_to_end_test();
    if (failures != 0) {
        std::cerr << failures << " llvm_lib test(s) failed.\n";
        return 1;
    }
    return 0;
}
