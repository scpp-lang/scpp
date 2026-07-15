import scpp.movecheck;
import scpp.parser;
import scpp.ast;

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
        if (module_name != "std") return std::nullopt;
        std::string partition_name = key.substr(colon + 1);
        std::filesystem::path module_path(SCPP_STDLIB_STD_MODULE_PATH);
        std::filesystem::path candidate =
            module_path.parent_path() / partition_name /
            (module_path.stem().string() + "_" + partition_name + module_path.extension().string());
        if (!std::filesystem::exists(candidate)) return std::nullopt;
        return candidate.string();
    }

    const std::string* module_path(const std::string& name) const {
        static const std::string std_module = SCPP_STDLIB_STD_MODULE_PATH;
        if (name == "std") return &std_module;
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

std::optional<std::string> move_error_message(std::string_view source) {
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
    } catch (const scpp::ParseError& e) {
        return e.what();
    } catch (const scpp::DataflowError& e) {
        return e.what();
    }
    return std::nullopt;
}

bool throws_move_error(std::string_view source) {
    return move_error_message(source).has_value();
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

void test_range_for_const_reference_rejects_mutation() {
    cases_run++;
    expect(throws_move_error(
               "int main() {\n"
               "    int values[2];\n"
               "    for (const auto& value : values) {\n"
               "        value = 1;\n"
               "    }\n"
               "    return 0;\n"
               "}\n"),
           "range_for_const_reference_rejects_mutation: expected mutation through const auto& to be rejected");
}

void test_mutable_reborrow_is_allowed_while_nested() {
    cases_run++;
    expect(!throws_move_error(
               "int main() {\n"
               "    int values[2];\n"
               "    int& whole = values[0];\n"
               "    {\n"
               "        int& nested = whole;\n"
               "        nested = 1;\n"
               "    }\n"
               "    whole = 2;\n"
               "    return 0;\n"
               "}\n"),
           "mutable_reborrow_is_allowed_while_nested: expected nested reborrow to pass");
}

void test_mutable_reborrow_allows_parent_read_while_live() {
    cases_run++;
    expect(!throws_move_error(
               "int main() {\n"
               "    int value = 1;\n"
               "    int& whole = value;\n"
               "    const int& nested = whole;\n"
               "    return whole + nested;\n"
               "}\n"),
           "mutable_reborrow_allows_parent_read_while_live: expected reads through lender and child to be allowed");
}

void test_mutable_reborrow_rejects_parent_write_while_live() {
    cases_run++;
    expect(throws_move_error(
               "int main() {\n"
               "    int values[2];\n"
               "    int& whole = values[0];\n"
               "    int& nested = whole;\n"
               "    whole = 1;\n"
               "    return nested;\n"
               "}\n"),
           "mutable_reborrow_rejects_parent_write_while_live: expected parent write during live reborrow to be rejected");
}

void test_mutable_reborrow_parent_becomes_usable_after_scope() {
    cases_run++;
    expect(!throws_move_error(
               "import std;\n"
               "int main() {\n"
               "    int values[2];\n"
               "    std::span<int> s = values;\n"
               "    {\n"
               "        int& nested = s[0];\n"
               "        nested = 1;\n"
               "    }\n"
               "    s[0] = 2;\n"
               "    return 0;\n"
               "}\n"),
           "mutable_reborrow_parent_becomes_usable_after_scope: expected lender to become usable after child scope ends");
}

void test_range_for_mutable_reference_over_span_is_accepted() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "int main() {\n"
        "    int values[2];\n"
        "    std::span<int> s = values;\n"
        "    for (auto& value : s) {\n"
        "        value = 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    expect(!error.has_value(),
           "range_for_mutable_reference_over_span_is_accepted: expected mutable span iteration to pass movecheck" +
               (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

void test_range_for_const_reference_over_span_rejects_mutation() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "int main() {\n"
               "    int values[2];\n"
               "    std::span<int> s = values;\n"
               "    for (const auto& value : s) {\n"
               "        value = 1;\n"
               "    }\n"
               "    return 0;\n"
               "}\n"),
           "range_for_const_reference_over_span_rejects_mutation: expected const span iteration mutation to be rejected");
}

void test_non_const_method_call_through_const_reference_reports_clear_diagnostic() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "class Counter {\n"
        "private:\n"
        "    int value{};\n"
        "public:\n"
        "    Counter(int start) {\n"
        "        this->value = start;\n"
        "        return;\n"
        "    }\n"
        "    virtual ~Counter() { return; }\n"
        "    void bump() {\n"
        "        this->value = this->value + 1;\n"
        "        return;\n"
        "    }\n"
        "};\n"
        "void mutate(const Counter& c) {\n"
        "    c.bump();\n"
        "    return;\n"
        "}\n");
    expect(error.has_value() && error->find("cannot call non-const member function 'bump'") != std::string::npos,
           "non_const_method_call_through_const_reference_reports_clear_diagnostic: expected a const receiver "
           "diagnostic, got '" +
              (error.has_value() ? *error : std::string("<no error>")) + "'");
}

void test_std_string_const_reference_mutation_reports_clear_diagnostic() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "void mutate(const std::string& text) {\n"
        "    text.append(\"!\");\n"
        "    return;\n"
        "}\n");
    expect(error.has_value() && error->find("cannot call non-const member function 'append'") != std::string::npos,
           "std_string_const_reference_mutation_reports_clear_diagnostic: expected a const receiver diagnostic, got '" +
              (error.has_value() ? *error : std::string("<no error>")) + "'");
}

void test_derived_constructor_requires_explicit_base_initializer_without_default_base_ctor() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "class Base {\n"
        "public:\n"
        "    Base(int seed) { return; }\n"
        "    virtual ~Base() { return; }\n"
        "};\n"
        "class Derived : public Base {\n"
        "public:\n"
        "    ~Derived() override { return; }\n"
        "    Derived() { return; }\n"
        "};\n");
    expect(error.has_value() && error->find("must initialize its direct base class 'Base'") != std::string::npos,
           "derived_constructor_requires_explicit_base_initializer_without_default_base_ctor: expected base-init "
           "diagnostic, got '" +
              (error.has_value() ? *error : std::string("<no error>")) + "'");
}

void test_explicit_base_initializer_satisfies_nondefault_base_ctor() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "class Base {\n"
        "public:\n"
        "    Base(int seed) { return; }\n"
        "    virtual ~Base() { return; }\n"
        "};\n"
        "class Derived : public Base {\n"
        "public:\n"
        "    ~Derived() override { return; }\n"
        "    Derived(int seed) : Base{seed} { return; }\n"
        "};\n");
    expect(!error.has_value(),
           "explicit_base_initializer_satisfies_nondefault_base_ctor: expected program to pass movecheck" +
              (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

} // namespace

int main() {
    run_test_case_files();
    test_mutable_reborrow_is_allowed_while_nested();
    test_mutable_reborrow_allows_parent_read_while_live();
    test_mutable_reborrow_rejects_parent_write_while_live();
    test_mutable_reborrow_parent_becomes_usable_after_scope();
    test_range_for_const_reference_rejects_mutation();
    test_range_for_mutable_reference_over_span_is_accepted();
    test_range_for_const_reference_over_span_rejects_mutation();
    test_non_const_method_call_through_const_reference_reports_clear_diagnostic();
    test_std_string_const_reference_mutation_reports_clear_diagnostic();
    test_derived_constructor_requires_explicit_base_initializer_without_default_base_ctor();
    test_explicit_base_initializer_satisfies_nondefault_base_ctor();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All move-check tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
