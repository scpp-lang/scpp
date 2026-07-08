import scpp.movecheck;
import scpp.parser;
import scpp.ast;

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// SCPP_MOVETEST_SOURCE_DIR is injected by CMake (see the movecheck_test
// target in the top-level CMakeLists.txt) and points at
// tests/movetest_source, so this binary finds its fixtures regardless of
// the working directory it's run from.
#ifndef SCPP_MOVETEST_SOURCE_DIR
#error "SCPP_MOVETEST_SOURCE_DIR must be defined by the build"
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

// Trims trailing whitespace/newlines, since `.expected` files end in `\n`.
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
        s.pop_back();
    }
    return s;
}

class TestModuleCache {
public:
    const scpp::Program& resolve(const std::string& module_name) {
        auto it = cache_.find(module_name);
        if (it != cache_.end()) return it->second;
        const std::string* path = module_path(module_name);
        if (path == nullptr) throw std::runtime_error("unknown test module '" + module_name + "'");
        scpp::Program parsed = scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program& { return resolve(name); },
            [this](const std::string& key) -> scpp::Program { return resolve_partition(key); });
        auto [inserted, _] = cache_.emplace(module_name, std::move(parsed));
        return inserted->second;
    }

    scpp::Program resolve_partition(const std::string& key) {
        const std::string* path = module_path(key);
        if (path == nullptr) throw std::runtime_error("unknown test partition '" + key + "'");
        return scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program& { return resolve(name); },
            [this](const std::string& nested_key) -> scpp::Program { return resolve_partition(nested_key); });
    }

private:
    const std::string* module_path(const std::string& name) const {
        static const std::string std_module = SCPP_STDLIB_STD_MODULE_PATH;
        static const std::string std_string_module = SCPP_STDLIB_STD_STRING_MODULE_PATH;
        static const std::string std_memory_module = SCPP_STDLIB_STD_MEMORY_MODULE_PATH;
        if (name == "std") return &std_module;
        if (name == "std:string") return &std_string_module;
        static const std::string std_functional_module = SCPP_STDLIB_STD_FUNCTIONAL_MODULE_PATH;
        if (name == "std:memory") return &std_memory_module;
        if (name == "std:functional") return &std_functional_module;
        static const std::string std_thread_module = SCPP_STDLIB_STD_THREAD_MODULE_PATH;
        if (name == "std:thread") return &std_thread_module;
        return nullptr;
    }

    std::unordered_map<std::string, scpp::Program> cache_;
};

scpp::Program parse_with_std_imports(std::string_view source) {
    TestModuleCache cache;
    return scpp::parse(
        source, [&cache](const std::string& name) -> const scpp::Program& { return cache.resolve(name); },
        [&cache](const std::string& key) -> scpp::Program { return cache.resolve_partition(key); });
}

bool throws_move_error(std::string_view source) {
    try {
        scpp::Program program = parse_with_std_imports(source);
        // ch05 §5.11: monomorphize_generics must run before check_moves,
        // exactly like the real pipeline (driver.cppm's
        // emit_object_file_for_program) -- a generic function's call
        // site is only ever type-correct against a witness-typed
        // signature *before* this rewrite; concept-satisfaction
        // rejection also only happens here, so a movetest_source case
        // exercising either would otherwise never see it.
        scpp::monomorphize_generics(program);
        scpp::check_moves(program);
    } catch (const scpp::DataflowError&) {
        return true;
    }
    return false;
}

// Runs every `<name>.scpp` case file under SCPP_MOVETEST_SOURCE_DIR against
// its paired `<name>.expected` file, which contains exactly "ok" (the move
// checker must accept the program) or "error" (it must reject it with a
// DataflowError). Adding a new case is just dropping in 2 new files -- no
// changes to this file or a rebuild of the test harness are needed, just
// re-running the already-built binary.
void run_test_case_files() {
    std::filesystem::path dir(SCPP_MOVETEST_SOURCE_DIR);
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

        std::string expected = trim(read_file(expected_path));
        if (expected != "ok" && expected != "error") {
            expect(false, case_name + ": .expected must contain 'ok' or 'error', got '" + expected + "'");
            continue;
        }

        cases_run++;
        bool threw = throws_move_error(read_file(source_path));
        bool expected_to_throw = expected == "error";
        expect(threw == expected_to_throw,
               case_name + ": expected " + expected + " but move check " + (threw ? "rejected" : "accepted") +
                   " the program");
    }
}

} // namespace

int main() {
    run_test_case_files();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All move-check tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
