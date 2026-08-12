import scpp.compiler.codegen;
import scpp.compiler.movecheck;
import scpp.constexpression;
import scpp.parser;
import scpp.ast;
import std;

// SCPP_CODEGEN_TEST_SOURCE_DIR is injected by CMake (see the codegen_test
// target in the top-level CMakeLists.txt) and points at
// tests/codegentest_source, so this binary finds its fixtures regardless of
// the working directory it's run from.
#ifndef SCPP_CODEGEN_TEST_SOURCE_DIR
#error "SCPP_CODEGEN_TEST_SOURCE_DIR must be defined by the build"
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

std::string join_names(const std::vector<std::string>& names) {
    std::string joined;
    for (const std::string& name : names) {
        if (!joined.empty()) joined += ", ";
        joined += name;
    }
    return joined;
}

std::string read_file(const std::filesystem::path& path) {    std::ifstream file(path);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

class TestModuleCache {
public:
    const scpp::Program& resolve(const std::string& module_name) {
        auto it = cache_.find(module_name);
        if (it != cache_.end()) return it->second;
        const std::string* path = module_path(module_name);
        if (path == nullptr) throw std::runtime_error("unknown test module '" + module_name + "'");
        auto parsed_result = scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program* { return &resolve(name); },
            [this](const std::string& key) -> scpp::Program { return resolve_partition(key); });
        if (!parsed_result.has_value()) throw std::move(parsed_result).error();
        auto [inserted, _] = cache_.emplace(module_name, std::move(parsed_result.value()));
        return inserted->second;
    }

    scpp::Program resolve_partition(const std::string& key) {
        std::optional<std::string> path = infer_partition_path(key);
        if (!path.has_value()) throw std::runtime_error("unknown test partition '" + key + "'");
        auto parsed_result = scpp::parse(
            read_file(*path), [this](const std::string& name) -> const scpp::Program* { return &resolve(name); },
            [this](const std::string& nested_key) -> scpp::Program { return resolve_partition(nested_key); });
        if (!parsed_result.has_value()) throw std::move(parsed_result).error();
        return std::move(parsed_result.value());
    }

private:
    std::optional<std::string> infer_partition_path(const std::string& key) const {
        std::size_t colon = key.find(':');
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

std::expected<scpp::Program, scpp::ParseError> try_parse_with_std_imports(std::string_view source) {
    TestModuleCache cache;
    return scpp::parse(
        source, [&cache](const std::string& name) -> const scpp::Program* { return &cache.resolve(name); },
        [&cache](const std::string& key) -> scpp::Program { return cache.resolve_partition(key); });
}

scpp::Program parse_with_std_imports(std::string_view source) {
    auto result = try_parse_with_std_imports(source);
    if (!result.has_value()) throw std::move(result).error();
    return std::move(result.value());
}

// generate_ir's pipeline touches four independent std::expected-returning
// stages (scpp::parse, scpp::monomorphize_generics, scpp::fold_immediate_calls,
// scpp::Codegen::generate), each with its own distinct error type. Since the
// call site itself already knows which stage it is calling at each step,
// preserving "which kind of error" is just a matter of tagging the failure
// with the stage's name at its own return site -- no variant or shared
// error-kind enum is needed the way driver.cppm's DriverErrorKind is, because
// there's no single function here that internally absorbs all four error
// types and needs to report the tag back up through a shared return type.
struct GenerateIrError {
    std::string kind; // "ParseError" | "DataflowError" | "ConstexprError" | "CodegenError"
    std::string message;
    // Only a CodegenError carries one; the point of recording it is that a
    // diagnostic is only as good as the position it blames (compiler bug
    // #7 -- an IR-invariant violation used to be reported once per module,
    // at whatever position the last-lowered function left behind).
    int line = 0;
    std::string source_path;
};

std::expected<std::string, GenerateIrError> try_generate_ir(std::string_view source) {
    auto parse_result = try_parse_with_std_imports(source);
    if (!parse_result.has_value()) return std::unexpected(GenerateIrError{"ParseError", parse_result.error().what(), 0, {}});
    scpp::Program program = std::move(parse_result.value());
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value())
        return std::unexpected(GenerateIrError{"DataflowError", monomorphize_result.error().what(), 0, {}});
    // ch05 §9.4: resolves every array bound (and other constant-expression
    // context, e.g. `alignas`) before codegen ever reads a type's layout --
    // codegen itself never evaluates constant expressions, only the
    // already-resolved `Type::array_size`. Mirrors driver.cppm's own
    // pipeline ordering (monomorphize_generics -> fold_immediate_calls ->
    // ... -> codegen).
    auto fold_result = scpp::fold_immediate_calls(program);
    if (!fold_result.has_value())
        return std::unexpected(GenerateIrError{"ConstexprError", fold_result.error().what(), 0, {}});
    scpp::Codegen codegen("test_module");
    auto generate_result = codegen.generate(program);
    if (!generate_result.has_value())
        return std::unexpected(GenerateIrError{"CodegenError", generate_result.error().what(),
                                              generate_result.error().loc.line,
                                              generate_result.error().loc.source_path_text()});
    return codegen.module_ir();
}

// Splits on " | ", used by the `sequence:` and `count_at_least:` assertion
// kinds to separate their pipe-delimited operands.
std::vector<std::string> split_pipe(const std::string& s) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        std::size_t bar = s.find(" | ", start);
        if (bar == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, bar - start));
        start = bar + 3;
    }
    return parts;
}

// One parsed line of a `.expected` file. See the comment on
// run_test_case_files() below for the supported assertion kinds and syntax.
struct Assertion {
    std::string kind;
    std::vector<std::string> args;
};

std::vector<Assertion> parse_expected(const std::string& content) {
    std::vector<Assertion> assertions;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        std::size_t colon = line.find(": ");
        std::string kind = colon == std::string::npos ? "" : line.substr(0, colon);
        std::string rest = colon == std::string::npos ? "" : line.substr(colon + 2);
        if (kind == "contains" || kind == "throws") {
            assertions.push_back(Assertion{kind, {rest}});
        } else if (kind == "sequence" || kind == "count_at_least") {
            assertions.push_back(Assertion{kind, split_pipe(rest)});
        } else {
            assertions.push_back(Assertion{"__malformed__", {line}});
        }
    }
    return assertions;
}

// Checks one non-`throws` assertion against `ir`, reporting via `expect`.
void check_ir_assertion(const Assertion& assertion, const std::string& ir, const std::string& case_name) {
    if (assertion.kind == "contains") {
        const std::string& needle = assertion.args[0];
        expect(ir.find(needle) != std::string::npos, case_name + ": expected IR to contain '" + needle + "'");
        return;
    }
    if (assertion.kind == "sequence") {
        // Each marker must be found in order; the search for the next one
        // starts right after the previous match ends, so this also
        // correctly skips an earlier, unrelated occurrence of a later
        // marker (e.g. a loop preheader's branch to `while.cond`, which
        // must be distinguished from the loop body's own back-edge).
        std::size_t pos = 0;
        for (const std::string& marker : assertion.args) {
            std::size_t found = ir.find(marker, pos);
            if (found == std::string::npos) {
                expect(false, case_name + ": expected to find '" + marker +
                                  "' after the previous marker in the sequence");
                return;
            }
            pos = found + marker.size();
        }
        return;
    }
    if (assertion.kind == "count_at_least") {
        int min_count = std::stoi(assertion.args[0]);
        const std::string& needle = assertion.args[1];
        int count = 0;
        std::size_t pos = 0;
        while (true) {
            std::size_t found = ir.find(needle, pos);
            if (found == std::string::npos) break;
            count++;
            pos = found + needle.size();
        }
        expect(count >= min_count, case_name + ": expected at least " + std::to_string(min_count) +
                                        " occurrence(s) of '" + needle + "', found " + std::to_string(count));
        return;
    }
    expect(false, case_name + ": malformed .expected line: '" + assertion.args[0] + "'");
}

// Runs every `<name>.scpp` case file under SCPP_CODEGEN_TEST_SOURCE_DIR
// against its paired `<name>.expected` file. Each non-blank line of
// `.expected` is one assertion against the generated IR (as text), except
// `throws:`, which instead asserts that parsing/codegen never produces IR
// at all:
//   contains: <substring>              -- the IR must contain this exact
//                                          substring somewhere.
//   sequence: <m1> | <m2> | ...        -- markers must appear in this
//                                          order (each search starts right
//                                          after the previous marker's own
//                                          match ends).
//   count_at_least: <n> | <substring>  -- substring occurs >= n times
//                                          (non-overlapping count).
//   throws: ParseError | DataflowError |
//           CodegenError | ConstexprError -- parsing (or, if parsing
//                                          succeeds, codegen) must fail
//                                          with exactly this error kind;
//                                          must be the only line in the
//                                          file.
// Adding a new case is just dropping in 2 new files -- no changes to this
// file or a rebuild of the test harness are needed, just re-running the
// already-built binary.
void run_test_case_files() {
    std::filesystem::path dir(SCPP_CODEGEN_TEST_SOURCE_DIR);
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

        std::vector<Assertion> assertions = parse_expected(read_file(expected_path));
        if (assertions.empty()) {
            expect(false, case_name + ": .expected has no assertions");
            continue;
        }

        cases_run++;
        std::string source = read_file(source_path);

        if (assertions[0].kind == "throws") {
            expect(assertions.size() == 1, case_name + ": 'throws:' must be the only line in .expected");
            const std::string& expected_type = assertions[0].args[0];

            auto ir_result = try_generate_ir(source);
            std::string actual = ir_result.has_value() ? "none" : ir_result.error().kind;
            expect(actual == expected_type,
                   case_name + ": expected " + expected_type + " to be thrown, got " + actual);
            continue;
        }

        auto ir_result = try_generate_ir(source);
        if (!ir_result.has_value()) {
            expect(false, case_name + ": unexpectedly failed with " + ir_result.error().kind + ": " +
                              ir_result.error().message);
            continue;
        }
        for (const Assertion& assertion : assertions) {
            check_ir_assertion(assertion, ir_result.value(), case_name);
        }
    }
}

// The two tests below exercise scpp::Codegen::generate's
// std::expected<llvm::LLVMModuleRef, CodegenError> API shape directly,
// without going through try_generate_ir's tagged-error convenience wrapper --
// mirroring parser_test.cpp's test_parse_returns_engaged_expected_on_success/
// test_parse_returns_disengaged_expected_on_failure_without_throwing and
// movecheck_test.cpp's analogous pair, added when parser.cppm/DataflowError
// made this same exceptions -> std::expected transition.
void test_generate_returns_engaged_expected_on_success() {
    cases_run++;
    scpp::Program program = parse_with_std_imports(
        "int main() {\n"
        "    return 0;\n"
        "}\n");
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) throw std::move(monomorphize_result).error();
    auto fold_result = scpp::fold_immediate_calls(program);
    if (!fold_result.has_value()) throw std::move(fold_result).error();
    scpp::Codegen codegen("test_module");
    auto result = codegen.generate(program);
    expect(result.has_value(), "generate_returns_engaged_expected_on_success: expected generate()'s has_value() "
                                "to be true");
}

void test_generate_returns_disengaged_expected_on_failure_without_throwing() {
    cases_run++;
    // No try/catch here at all -- if scpp::Codegen::generate still threw
    // instead of returning std::expected, this call itself would already
    // have aborted the test binary before reaching any of the expect()
    // calls below, since nothing in this function catches exceptions.
    scpp::Program program = parse_with_std_imports("int main() { return unknown(); }\n");
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) throw std::move(monomorphize_result).error();
    auto fold_result = scpp::fold_immediate_calls(program);
    if (!fold_result.has_value()) throw std::move(fold_result).error();
    scpp::Codegen codegen("test_module");
    auto result = codegen.generate(program);
    expect(!result.has_value(),
           "generate_returns_disengaged_expected_on_failure_without_throwing: expected has_value() to be false");
    if (result.has_value()) return;
    const scpp::CodegenError& error = result.error();
    expect(error.loc.is_known(),
           "generate_returns_disengaged_expected_on_failure_without_throwing: expected a known error location");
    expect(std::string(error.what()).size() > 0,
           "generate_returns_disengaged_expected_on_failure_without_throwing: expected a non-empty diagnostic "
           "message");
}

// Exercises scpp.constexpression's public API directly against its new
// std::expected<T, ConstexprError> return type (rather than only indirectly,
// via the .scpp/.expected `throws: ConstexprError` case above), to confirm
// callers can report both success and failure without relying on
// exceptions -- the eventual self-hosting requirement for this engine.
void run_constexpr_engine_direct_api_tests() {
    {
        std::string case_name = "fold_immediate_calls_succeeds_for_well_formed_consteval_call";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "consteval int answer() { return 42; }\n"
            "int main() { return answer(); }\n");
        auto result = scpp::fold_immediate_calls(program);
        expect(result.has_value(),
               case_name + ": expected fold_immediate_calls to return a value instead of an error");
    }

    {
        std::string case_name = "fold_immediate_calls_reports_error_for_runaway_consteval_recursion";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "consteval int loops_forever() { return loops_forever(); }\n"
            "int main() { return loops_forever(); }\n");
        auto result = scpp::fold_immediate_calls(program);
        expect(!result.has_value(), case_name + ": expected fold_immediate_calls to return std::unexpected "
                                                  "instead of throwing for runaway consteval recursion");
    }
}

// PR #414: the switch end block used to be marked `unreachable` based on a
// syntactic peek at each case's *last* statement (`ends_with_break`), which
// cannot see a `break;` nested inside an `if` or a block. When a switch had
// a `default` (so `end_block_reachable` started false) and every reaching
// `break;` was nested, codegen emitted `unreachable` at a block that control
// really does branch to -- a silent miscompile that traps or falls into
// undefined behavior at run time. The fix asks LLVM (LLVMGetFirstUse)
// whether anything actually branches there.
void run_switch_end_block_reachability_tests() {
    {
        // The pure codegen regression: this source parses fine on the
        // pre-#414 parser too (the case body is unbraced and ends in
        // `return`), so it isolates the reachability bug from the parser
        // change. The only way to reach the end block is the `break;`
        // nested inside the `if`.
        std::string case_name = "switch_end_reached_only_by_break_nested_in_if_is_not_unreachable";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int total = 0;\n"
                                  "    switch (1) {\n"
                                  "        case 1:\n"
                                  "            if (total == 0) {\n"
                                  "                break;\n"
                                  "            }\n"
                                  "            return 9;\n"
                                  "        default:\n"
                                  "            return 8;\n"
                                  "    }\n"
                                  "    return total;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (!ir.has_value()) return;
        expect(ir.value().find("unreachable") == std::string::npos,
               case_name + ": switch end block is reachable via the nested break, so no 'unreachable' should be "
                           "emitted anywhere in this function");
    }

    {
        // The same bug reached through a braced case body -- the form PR
        // #414's parser half newly admits.
        std::string case_name = "switch_end_reached_only_by_break_nested_in_block_is_not_unreachable";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int total = 0;\n"
                                  "    switch (1) {\n"
                                  "        case 1: {\n"
                                  "            if (total == 0) {\n"
                                  "                break;\n"
                                  "            }\n"
                                  "            return 9;\n"
                                  "        }\n"
                                  "        default: {\n"
                                  "            return 8;\n"
                                  "        }\n"
                                  "    }\n"
                                  "    return total;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (!ir.has_value()) return;
        expect(ir.value().find("unreachable") == std::string::npos,
               case_name + ": switch end block is reachable via the break nested in the braced case body");
    }

    {
        // The other direction: when the end block genuinely cannot be
        // reached (every case returns, and a `default` covers the rest),
        // `unreachable` must still be emitted. Without this the fix could
        // "pass" by simply never emitting `unreachable` again.
        std::string case_name = "switch_end_is_unreachable_when_every_case_returns";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    switch (1) {\n"
                                  "        case 1:\n"
                                  "            return 1;\n"
                                  "        default:\n"
                                  "            return 0;\n"
                                  "    }\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (!ir.has_value()) return;
        expect(ir.value().find("unreachable") != std::string::npos,
               case_name + ": no case branches to the switch end block, so it must still be marked 'unreachable'");
    }

    {
        // A top-level `break;` still keeps the end block reachable -- the
        // pre-existing behavior the old `ends_with_break` peek got right,
        // guarded here so the rewrite did not regress it.
        std::string case_name = "switch_end_reached_by_top_level_break_is_not_unreachable";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int total = 0;\n"
                                  "    switch (1) {\n"
                                  "        case 1:\n"
                                  "            break;\n"
                                  "        default:\n"
                                  "            return 8;\n"
                                  "    }\n"
                                  "    return total;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (!ir.has_value()) return;
        expect(ir.value().find("unreachable") == std::string::npos,
               case_name + ": a top-level break keeps the switch end block reachable");
    }
}

// Slices out one LLVM function definition, so a count assertion can be
// scoped to the function under test rather than to the whole module
// (where the same callee may also appear in a vtable or another
// function). Returns an empty string if there is no such definition.
std::string function_ir(const std::string& ir, const std::string& function_name) {
    const std::string marker = "@" + function_name + "(";
    std::size_t begin = ir.find("define ");
    while (begin != std::string::npos) {
        std::size_t signature_end = ir.find('\n', begin);
        if (signature_end == std::string::npos) return {};
        std::size_t name_at = ir.find(marker, begin);
        if (name_at != std::string::npos && name_at < signature_end) {
            std::size_t end = ir.find("\n}", begin);
            return end == std::string::npos ? ir.substr(begin) : ir.substr(begin, end - begin);
        }
        begin = ir.find("define ", signature_end);
    }
    return {};
}

// Counts non-overlapping occurrences of `needle` in `haystack`.
std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = haystack.find(needle);
    while (pos != std::string::npos) {
        count++;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

// Compiler bug #4: Codegen::locals_ used to be a flat map keyed by the
// *source name*, so an inner declaration overwrote the outer namesake's
// slot and pop_scope's erase-by-name then deleted the outer declaration's
// entry along with the inner one. Two distinct failures followed: a use of
// the outer variable after the inner scope was rejected with "use of
// undeclared variable", and -- worse -- the outer object's destructor and
// unique_ptr teardown were silently dropped, because the scope-exit walk
// could no longer find the storage to clean up. locals_ is now keyed by
// LocalId (name resolution's declaration identity, the same model
// movecheck adopted in #427), so each declaration owns its own slot.
//
// Every case below is rejected by the pre-fix compiler, so they all have
// teeth. Note these programs only became reachable at all once bug #2 was
// fixed -- before that, movecheck rejected them earlier in the pipeline.
void run_local_shadowing_tests() {
    {
        // The base shape: the outer local must still be usable, for both
        // reading and writing, after an inner shadow's scope has ended.
        std::string case_name = "outer_local_is_usable_after_an_inner_shadow_scope_ends";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int x = 1;\n"
                                  "    {\n"
                                  "        int x = 10;\n"
                                  "        x = x + 1;\n"
                                  "    }\n"
                                  "    x = x + 2;\n"
                                  "    return x;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // Distinct declarations must get distinct storage, not one aliased
        // slot -- a fix that resolved names correctly but reused the
        // allocation would still be wrong. Differing types make that
        // directly observable in the IR: an aliased slot could only have
        // one of the two LLVM types.
        std::string case_name = "shadow_with_a_different_type_gets_its_own_storage";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int x = 1;\n"
                                  "    {\n"
                                  "        double x = 2.5;\n"
                                  "        x = x + 1.0;\n"
                                  "    }\n"
                                  "    x = x + 2;\n"
                                  "    return x;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            expect(ir.value().find("alloca i32") != std::string::npos,
                   case_name + ": expected the outer `int x` to keep its own i32 alloca");
            expect(ir.value().find("alloca double") != std::string::npos,
                   case_name + ": expected the inner `double x` to get its own separate double alloca");
        }
    }

    {
        // A shadow of a *parameter*, which lives in the function's own
        // outermost binding rather than in a pushed block scope, so it
        // exercises a different declaration path.
        std::string case_name = "local_may_shadow_a_parameter_and_the_parameter_survives";
        cases_run++;
        auto ir = try_generate_ir("int scale(int v) {\n"
                                  "    {\n"
                                  "        double v = 0.5;\n"
                                  "        v = v + 1.0;\n"
                                  "    }\n"
                                  "    return v * 2;\n"
                                  "}\n"
                                  "int main() { return scale(21); }\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            expect(ir.value().find("alloca double") != std::string::npos,
                   case_name + ": expected the shadowing local to get storage of its own type");
        }
    }

    {
        // Nesting depth: erase-by-name lost one slot per level, so a chain
        // of shadows failed at whichever level was popped first.
        std::string case_name = "shadowing_works_at_multiple_nesting_depths";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int x = 1;\n"
                                  "    {\n"
                                  "        int x = 2;\n"
                                  "        {\n"
                                  "            int x = 3;\n"
                                  "            {\n"
                                  "                int x = 4;\n"
                                  "                x = x + 1;\n"
                                  "            }\n"
                                  "            x = x + 1;\n"
                                  "        }\n"
                                  "        x = x + 1;\n"
                                  "    }\n"
                                  "    return x;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // if/else branches and loop bodies are pushed scopes too.
        std::string case_name = "shadowing_inside_if_else_branches_and_a_loop_body";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int x = 1;\n"
                                  "    if (x == 1) {\n"
                                  "        long x = 5;\n"
                                  "        x = x + 1;\n"
                                  "    } else {\n"
                                  "        double x = 6.0;\n"
                                  "        x = x + 1.0;\n"
                                  "    }\n"
                                  "    for (int i = 0; i < 2; i++) {\n"
                                  "        char x = 'a';\n"
                                  "        x = x;\n"
                                  "    }\n"
                                  "    return x;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // The miscompile half of the bug, and the reason this was worse
        // than a spurious rejection. Shadowing a class-typed local erased
        // the outer object's slot, so the outer scope's exit could no
        // longer find any storage to destroy and silently emitted *no*
        // destructor call for it -- a broken RAII invariant with no
        // diagnostic at all. Both objects must be destroyed.
        std::string case_name = "shadowed_outer_object_destructor_is_still_emitted";
        cases_run++;
        auto ir = try_generate_ir("class Tracked {\n"
                                  "  public:\n"
                                  "    int tag = 0;\n"
                                  "    virtual ~Tracked() { tag = 0; }\n"
                                  "};\n"
                                  "void shadowed() {\n"
                                  "    Tracked a{};\n"
                                  "    {\n"
                                  "        Tracked a{};\n"
                                  "        a.tag = 10;\n"
                                  "    }\n"
                                  "    a.tag = 1;\n"
                                  "}\n"
                                  "int main() { shadowed(); return 0; }\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string shadowed_ir = function_ir(ir.value(), "shadowed");
            expect(!shadowed_ir.empty(), case_name + ": expected a definition of `shadowed` in the module IR");
            std::size_t destructor_calls = count_occurrences(shadowed_ir, "call void @Tracked_delete(");
            expect(destructor_calls == 2, case_name + ": expected the shadowed outer object and the inner one to "
                                                      "each be destroyed, got " +
                                              std::to_string(destructor_calls) + " destructor call(s)");
        }
    }

    {
        // The same shape for unique_ptr's scope-exit free.
        std::string case_name = "shadowed_outer_unique_ptr_is_still_freed_at_its_own_scope_exit";
        cases_run++;
        auto ir = try_generate_ir("import std;\n"
                                  "int main() {\n"
                                  "    std::unique_ptr<int> p = std::make_unique<int>(1);\n"
                                  "    {\n"
                                  "        std::unique_ptr<int> p = std::make_unique<int>(2);\n"
                                  "    }\n"
                                  "    return 0;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(!main_ir.empty(), case_name + ": expected a definition of `main` in the module IR");
            std::size_t releases = count_occurrences(main_ir, "unique_ptr.int_delete");
            expect(releases == 2, case_name + ": expected both the shadowed outer unique_ptr and the inner one to "
                                              "be released at their own scope exits, got " +
                                      std::to_string(releases) + " teardown call(s)");
        }
    }

    {
        // A `[&]` capture reads the *enclosing* declaration, and codegen
        // synthesizes the identifier node for it rather than walking a
        // parsed one, so that node has to carry the capture's own resolved
        // declaration. When it did not, every blanket-capture lambda
        // failed with "use of undeclared variable".
        std::string case_name = "blanket_reference_capture_binds_the_enclosing_declaration";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int total = 0;\n"
                                  "    int seed = 10;\n"
                                  "    auto add = [&](int v) -> void { total = total + v + seed; };\n"
                                  "    add(1);\n"
                                  "    return total;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // A capture must bind the declaration in scope at the lambda, not
        // the outer namesake.
        std::string case_name = "capture_of_a_shadowing_local_binds_the_inner_declaration";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int total = 0;\n"
                                  "    {\n"
                                  "        double total = 1.5;\n"
                                  "        auto f = [&]() -> double { return total + 1.0; };\n"
                                  "        total = f();\n"
                                  "    }\n"
                                  "    return total;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            expect(ir.value().find("alloca double") != std::string::npos,
                   case_name + ": expected the captured inner `double total` to keep its own storage");
        }
    }

    {
        // Keying by declaration must not weaken the existing rule that a
        // name is unusable once its scope has ended -- only that
        // declaration's slot goes away now, not its outer namesake's.
        std::string case_name = "use_after_scope_end_is_still_rejected";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    {\n"
                                  "        int y = 1;\n"
                                  "        y = y + 1;\n"
                                  "    }\n"
                                  "    return y;\n"
                                  "}\n");
        expect(!ir.has_value(), case_name + ": expected a use of an out-of-scope local to still be rejected");
        if (!ir.has_value()) {
            // Diagnostics are the one place a LocalId must never surface:
            // they are written for a human reading their own source.
            expect(ir.error().message.find("'y'") != std::string::npos,
                   case_name + ": expected the diagnostic to name the source variable 'y', got: " + ir.error().message);
        }
    }

    {
        // The same pin for a name that was never declared at all, which
        // takes the other reporting path.
        std::string case_name = "undeclared_variable_diagnostic_names_the_source_identifier";
        cases_run++;
        auto ir = try_generate_ir("int main() { return nowhere; }\n");
        expect(!ir.has_value(), case_name + ": expected an undeclared variable to be rejected");
        if (!ir.has_value()) {
            expect(ir.error().message.find("'nowhere'") != std::string::npos,
                   case_name + ": expected the diagnostic to name 'nowhere', got: " + ir.error().message);
        }
    }
}

// Compiler bug #7. `return` was the only boundary in the language that
// handed a value to a declared type without ever asking whether the two
// types matched. Initialization and assignment ask it in
// check_store_type; a call argument asks it during overload resolution.
// A `return` did not, so `std::int64_t f() { int x = 5; return x; }`
// lowered a `ret i32` into an `i64` function and got no further than
// LLVM's own module verifier -- which runs once over the *finished*
// module, long after the offending statement was left behind, and so
// blamed whichever function happened to be lowered last (in practice a
// stdlib function the user never wrote).
void run_return_type_checking_tests() {
    {
        // The reported shape: a narrower integer value returned from a
        // wider-integer function. scpp has no implicit scalar
        // conversions (spec ch06), so this is an error, not a widening.
        std::string case_name = "return_int_from_int64_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("std::int64_t widen() {\n"
                                  "    int x = 5;\n"
                                  "    return x;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected an 'int' returned from an 'int64_t' function to be rejected");
        if (!ir.has_value()) {
            expect(ir.error().kind == "CodegenError",
                   case_name + ": expected the frontend to reject this, got: " + ir.error().kind);
            expect(ir.error().message.find("'widen'") != std::string::npos,
                   case_name + ": expected the diagnostic to name the offending function, got: " + ir.error().message);
            // The other half of the bug: the position. `return x;` is on
            // line 3 of the source above, and it is not in the stdlib.
            expect(ir.error().line == 3,
                   case_name + ": expected the diagnostic at the offending return (line 3), got line " +
                       std::to_string(ir.error().line));
            expect(ir.error().source_path.find("std_memory") == std::string::npos &&
                       ir.error().source_path.find("libs/std/") == std::string::npos,
                   case_name + ": expected the diagnostic to blame the user's own source, got path '" +
                       ir.error().source_path + "'");
        }
    }

    {
        // Not specific to integer widths: the gap was total, so a class
        // value returned from an `int` function was equally unchecked.
        std::string case_name = "return_class_value_from_int_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("class Box {\n"
                                  "public:\n"
                                  "    int v = 1;\n"
                                  "    virtual ~Box() { }\n"
                                  "};\n"
                                  "int returns_class() {\n"
                                  "    Box b{};\n"
                                  "    return b;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected a class value returned from an 'int' function to be rejected");
    }

    {
        // The other direction: a wider value narrowed into a narrower
        // return type is just as much a mismatch.
        std::string case_name = "return_int64_from_int_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("int narrows() {\n"
                                  "    std::int64_t x = 1;\n"
                                  "    return x;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected an 'int64_t' returned from an 'int' function to be rejected");
    }

    {
        std::string case_name = "return_int_from_bool_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("bool flag() {\n"
                                  "    int x = 1;\n"
                                  "    return x;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected an 'int' returned from a 'bool' function to be rejected");
    }

    {
        std::string case_name = "return_int_from_double_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("double scaled() {\n"
                                  "    int x = 1;\n"
                                  "    return x;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected an 'int' returned from a 'double' function to be rejected");
    }

    {
        // A bare `return;` in a value-returning function left the `ret`
        // operand missing entirely -- `ret void` in an `i32` function.
        std::string case_name = "bare_return_from_value_returning_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("int bare() { return; }\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected a bare 'return;' in an 'int' function to be rejected");
        if (!ir.has_value()) {
            expect(ir.error().message.find("'int'") != std::string::npos,
                   case_name + ": expected the diagnostic to name the declared return type, got: " +
                       ir.error().message);
        }
    }

    {
        // The mirror image, and the one shape the check originally still
        // missed: a void function's return branch evaluated the operand
        // for its side effects and then silently discarded it.
        std::string case_name = "returning_a_value_from_a_void_function_is_rejected";
        cases_run++;
        auto ir = try_generate_ir("void nothing() { return 5; }\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected a value returned from a 'void' function to be rejected");
        if (!ir.has_value()) {
            expect(ir.error().message.find("'void'") != std::string::npos,
                   case_name + ": expected the diagnostic to say the function returns void, got: " +
                       ir.error().message);
        }
    }

    {
        // ...while `return void_call();` -- returning another void-typed
        // expression -- stays legal, exactly as in real C++.
        std::string case_name = "returning_a_void_call_from_a_void_function_is_accepted";
        cases_run++;
        auto ir = try_generate_ir("void inner() { }\n"
                                  "void outer() { return inner(); }\n"
                                  "int main() { outer(); return 0; }\n");
        expect(ir.has_value(), case_name + ": expected 'return void_call();' to still compile, got: " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // Integer and float literals legitimately adapt to the target
        // type at every other boundary (codegen_value_for_target
        // const-folds them), and the return boundary must not break
        // that -- a literal is not a value of some other type.
        std::string case_name = "literal_returns_still_adapt_to_the_declared_return_type";
        cases_run++;
        auto ir = try_generate_ir("std::int64_t wide() { return 5; }\n"
                                  "double frac() { return 1.5; }\n"
                                  "bool yes() { return true; }\n"
                                  "int main() { return 0; }\n");
        expect(ir.has_value(), case_name + ": expected literal returns to still compile, got: " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            expect(ir.value().find("ret i64 5") != std::string::npos,
                   case_name + ": expected the integer literal to be folded to the declared i64 return type");
        }
    }

    {
        // Reference returns go through codegen_lvalue and yield a
        // pointer, and class-by-value returns go through
        // codegen_class_value_for_boundary. Both already produce exactly
        // the declared type; pin that so the new check never starts
        // rejecting them.
        std::string case_name = "reference_and_class_value_returns_are_still_accepted";
        cases_run++;
        auto ir = try_generate_ir("class Box {\n"
                                  "public:\n"
                                  "    int v = 7;\n"
                                  "    virtual ~Box() { }\n"
                                  "};\n"
                                  "Box& pick(Box& a) { return a; }\n"
                                  "Box make_box() {\n"
                                  "    Box b{};\n"
                                  "    return b;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(ir.has_value(), case_name + ": expected reference/class-value returns to still compile, got: " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // The diagnostic has to name the type the user actually wrote,
        // not codegen's lowered spelling of it -- verbatim_type_spelling,
        // the same source the store-boundary diagnostic uses.
        std::string case_name = "return_diagnostic_names_the_declared_return_type_verbatim";
        cases_run++;
        auto ir = try_generate_ir("std::int64_t widen() {\n"
                                  "    int x = 5;\n"
                                  "    return x;\n"
                                  "}\n"
                                  "int main() { return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected the mismatch to be rejected");
        if (!ir.has_value()) {
            expect(ir.error().message.find("int64_t") != std::string::npos,
                   case_name + ": expected the diagnostic to name 'int64_t', got: " + ir.error().message);
            expect(ir.error().message.find("static_cast") != std::string::npos,
                   case_name + ": expected the diagnostic to suggest an explicit static_cast, got: " +
                       ir.error().message);
        }
    }

    {
        // The store-boundary diagnostic next door used to end with "cast
        // expressions aren't implemented in this version yet", which is
        // no longer true -- static_cast<T>(...) compiles and runs. A
        // diagnostic that denies the existence of the only available fix
        // is worse than no hint at all.
        std::string case_name = "store_mismatch_diagnostic_suggests_static_cast";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int x = 5;\n"
                                  "    std::int64_t y = x;\n"
                                  "    return 0;\n"
                                  "}\n");
        expect(!ir.has_value(), case_name + ": expected the store mismatch to still be rejected");
        if (!ir.has_value()) {
            expect(ir.error().message.find("static_cast") != std::string::npos,
                   case_name + ": expected the diagnostic to suggest an explicit static_cast, got: " +
                       ir.error().message);
            expect(ir.error().message.find("aren't implemented") == std::string::npos,
                   case_name + ": the diagnostic still claims casts are unimplemented, got: " + ir.error().message);
        }
    }

    {
        // And the fix it suggests has to actually work, at both
        // boundaries.
        std::string case_name = "static_cast_satisfies_both_the_store_and_return_boundaries";
        cases_run++;
        auto ir = try_generate_ir("std::int64_t widen() {\n"
                                  "    int x = 5;\n"
                                  "    return static_cast<std::int64_t>(x);\n"
                                  "}\n"
                                  "int main() {\n"
                                  "    int x = 5;\n"
                                  "    std::int64_t y = static_cast<std::int64_t>(x);\n"
                                  "    return static_cast<int>(y) - 5;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected an explicit static_cast to satisfy both boundaries, got: " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }
}

// Compiler bug #5, uncovered by the bug #4 fix above. A virtual interface
// base is constructed exactly once, by the most-derived object, so codegen
// emits those base constructions at the *construction site* rather than
// inside the constructor. But the initializer expressions it emits there
// were written in the constructor's own member-initializer list and name
// the constructor's parameters -- which do not exist in the construction
// site's frame. While locals_ was keyed by name this silently resolved
// against whatever the construction site happened to have declared: it
// worked only when the caller had a same-named local of a compatible type,
// and otherwise picked up an unrelated variable. Codegen now evaluates the
// arguments first and binds them to the constructor's parameters for the
// duration, which is also real C++'s ordering.
void run_virtual_base_initializer_frame_tests() {
    const std::string diamond_source =
        "int seen = 0;\n"
        "int calls = 0;\n"
        "int next_value() { calls = calls + 1; return 7; }\n"
        "class [[scpp::interface]] IBase {\n"
        "  public:\n"
        "    IBase(int v) { seen = v; return; }\n"
        "    IBase() { return; }\n"
        "    virtual ~IBase() = default;\n"
        "    virtual void ping() = 0;\n"
        "};\n"
        "class [[scpp::interface]] ILeft : public virtual IBase {\n"
        "  public:\n"
        "    ~ILeft() override = default;\n"
        "};\n"
        "class Derived : public virtual ILeft {\n"
        "  public:\n"
        "    Derived(int amount) : IBase{amount} { return; }\n"
        "    ~Derived() override = default;\n"
        "    void ping() override { return; }\n"
        "};\n"
        "int main() {\n"
        "    Derived d{next_value()};\n"
        "    return seen;\n"
        "}\n";

    {
        // `amount` is Derived's parameter and nothing in `main` is spelled
        // that way, so the initializer can only be resolved in the
        // constructor's frame.
        std::string case_name = "virtual_base_initializer_resolves_against_the_constructors_own_parameters";
        cases_run++;
        auto ir = try_generate_ir(diamond_source);
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }

    {
        // The construction site now emits each argument once and reuses
        // the value for both the virtual base initializer and the
        // constructor call, so a side-effecting argument must not run
        // twice.
        std::string case_name = "constructor_arguments_are_evaluated_once_for_virtual_base_initialization";
        cases_run++;
        auto ir = try_generate_ir(diamond_source);
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(!main_ir.empty(), case_name + ": expected a definition of `main` in the module IR");
            std::size_t evaluations = count_occurrences(main_ir, "call i32 @next_value(");
            expect(evaluations == 1, case_name + ": expected the argument to be evaluated exactly once, got " +
                                         std::to_string(evaluations) + " evaluation(s)");
        }
    }
}

// Compiler bug #6: codegen_call gated constructor resolution for a
// class-construction expression on `!expr.args.empty() || expr.has_paren_init`.
// `has_paren_init` is only ever set by the parser on ExprKind::New nodes, so
// for a `ClassName{}` / `ClassName()` *expression* the guard was always false
// and no constructor was ever resolved: the temporary was zero-initialized and
// the user-declared default constructor never ran -- while its destructor still
// ran at scope exit, leaving an unbalanced object lifetime. Every other
// spelling of "construct a ClassName" already resolved unconditionally: the
// `ClassName v{};` VarDecl form, the bare `return {};` ValueInit form, and the
// compile-time evaluator (constexpression.cppm's evaluate_constructor_expr),
// which is why a consteval evaluation of the very same expression produced the
// right answer while codegen did not.
//
// Every case below fails against the pre-fix compiler.
void run_value_initialized_temporary_constructor_tests() {
    const std::string flagged_class =
        "class Flagged {\n"
        "  public:\n"
        "    Flagged() { this->flag = 7; return; }\n"
        "    virtual ~Flagged() = default;\n"
        "    int flag = 0;\n"
        "};\n";

    {
        // The reported shape: an empty-braced temporary bound to `auto`.
        std::string case_name = "value_initialized_temporary_runs_the_default_constructor";
        cases_run++;
        auto ir = try_generate_ir(flagged_class + "int main() {\n"
                                                  "    auto b = Flagged{};\n"
                                                  "    return b.flag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(!main_ir.empty(), case_name + ": expected a definition of `main` in the module IR");
            std::size_t constructions = count_occurrences(main_ir, "call void @Flagged_new(");
            expect(constructions == 1, case_name + ": expected the empty-braced temporary to run the default "
                                                   "constructor exactly once, got " +
                                           std::to_string(constructions) + " constructor call(s)");
        }
    }

    {
        // The declaration form already worked; the point is that both
        // spellings must now emit the same construction, not that either
        // one emits *a* call.
        std::string case_name = "declaration_and_expression_forms_construct_identically";
        cases_run++;
        auto ir = try_generate_ir(flagged_class + "int main() {\n"
                                                  "    Flagged a{};\n"
                                                  "    auto b = Flagged{};\n"
                                                  "    return a.flag + b.flag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(!main_ir.empty(), case_name + ": expected a definition of `main` in the module IR");
            std::size_t constructions = count_occurrences(main_ir, "call void @Flagged_new(");
            expect(constructions == 2, case_name + ": expected the declaration form and the expression form to each "
                                                   "run the default constructor, got " +
                                           std::to_string(constructions) + " constructor call(s)");
        }
    }

    {
        // The empty-parenthesized spelling reaches the same Call node with
        // an equally empty argument list.
        std::string case_name = "empty_parenthesized_temporary_runs_the_default_constructor";
        cases_run++;
        auto ir = try_generate_ir(flagged_class + "int main() {\n"
                                                  "    auto b = Flagged();\n"
                                                  "    return b.flag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            std::size_t constructions = count_occurrences(main_ir, "call void @Flagged_new(");
            expect(constructions == 1, case_name + ": expected `Flagged()` to run the default constructor exactly "
                                                   "once, got " +
                                           std::to_string(constructions) + " constructor call(s)");
        }
    }

    {
        // Argument position and return position both materialize the same
        // temporary through codegen_call, so both were equally broken.
        std::string case_name = "value_initialized_temporary_in_argument_and_return_position";
        cases_run++;
        auto ir = try_generate_ir(flagged_class + "int take(Flagged f) { return f.flag; }\n"
                                                  "Flagged produce() { return Flagged{}; }\n"
                                                  "int main() {\n"
                                                  "    Flagged p = produce();\n"
                                                  "    return take(Flagged{}) + p.flag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string produce_ir = function_ir(ir.value(), "produce");
            expect(!produce_ir.empty(), case_name + ": expected a definition of `produce` in the module IR");
            expect(count_occurrences(produce_ir, "call void @Flagged_new(") == 1,
                   case_name + ": expected `return Flagged{};` to run the default constructor");
            std::string main_ir = function_ir(ir.value(), "main");
            expect(count_occurrences(main_ir, "call void @Flagged_new(") == 1,
                   case_name + ": expected the argument-position temporary to run the default constructor");
        }
    }

    {
        // The worse half of the defect: the temporary skipped its
        // constructor but still ran its destructor, so an object that
        // acquires in its constructor and releases in its destructor would
        // release something it never acquired. Construction and destruction
        // must be balanced.
        std::string case_name = "value_initialized_temporary_balances_construction_and_destruction";
        cases_run++;
        auto ir = try_generate_ir("class Tracked {\n"
                                  "  public:\n"
                                  "    Tracked() { this->tag = 7; return; }\n"
                                  "    virtual ~Tracked() { this->tag = 0; }\n"
                                  "    int tag = 0;\n"
                                  "};\n"
                                  "void use() {\n"
                                  "    auto t = Tracked{};\n"
                                  "    t.tag = 1;\n"
                                  "}\n"
                                  "int main() { use(); return 0; }\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string use_ir = function_ir(ir.value(), "use");
            expect(!use_ir.empty(), case_name + ": expected a definition of `use` in the module IR");
            std::size_t constructions = count_occurrences(use_ir, "call void @Tracked_new(");
            std::size_t destructions = count_occurrences(use_ir, "call void @Tracked_delete(");
            expect(constructions == destructions,
                   case_name + ": expected construction and destruction to be balanced, got " +
                       std::to_string(constructions) + " constructor call(s) and " + std::to_string(destructions) +
                       " destructor call(s)");
            expect(constructions == 1, case_name + ": expected exactly one construction, got " +
                                           std::to_string(constructions));
        }
    }

    {
        // A class with no constructors at all must still take the
        // in-class field-initializer path rather than acquiring a
        // constructor call it does not have.
        std::string case_name = "constructorless_class_still_uses_its_in_class_field_initializers";
        cases_run++;
        auto ir = try_generate_ir("class Plain {\n"
                                  "  public:\n"
                                  "    virtual ~Plain() = default;\n"
                                  "    int a = 5;\n"
                                  "};\n"
                                  "int main() {\n"
                                  "    auto p = Plain{};\n"
                                  "    return p.a;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(count_occurrences(main_ir, "@Plain_new(") == 0,
                   case_name + ": a class with no constructors must not acquire a constructor call");
            expect(main_ir.find("store i32 5") != std::string::npos,
                   case_name + ": expected the in-class field initializer to still be emitted");
        }
    }

    {
        // The non-empty argument list already resolved before the fix and
        // must keep resolving to the same overload.
        std::string case_name = "argument_carrying_construction_expression_still_resolves";
        cases_run++;
        auto ir = try_generate_ir("class Two {\n"
                                  "  public:\n"
                                  "    Two() { this->a = 1; return; }\n"
                                  "    Two(int x) { this->a = x + 100; return; }\n"
                                  "    virtual ~Two() = default;\n"
                                  "    int a = 0;\n"
                                  "};\n"
                                  "int main() {\n"
                                  "    auto z = Two{};\n"
                                  "    auto n = Two{5};\n"
                                  "    return z.a + n.a;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(count_occurrences(main_ir, "@Two_new") == 2,
                   case_name + ": expected both the zero-argument and the one-argument construction to resolve");
        }
    }
}

// PR #416: ConstexprError gained a hand-written copy constructor. Nearly
// all of that PR is a mechanical struct -> class reshaping (adding
// `public:`, virtual destructors and brace-init) with no behavior delta,
// but this constructor is genuinely new *code*: scpp's std::runtime_error
// declares no copy constructor, so the base has to be rebuilt from what()
// rather than copy-initialized. If it dropped or mangled either the
// message or the location, every ConstexprError surfaced through
// std::expected would silently lose its diagnostic -- so assert both
// survive the copy that std::unexpected's converting constructor makes.
void run_constexpr_error_copy_tests() {
    std::string case_name = "constexpr_error_survives_copy_through_expected";
    cases_run++;
    scpp::Program program = parse_with_std_imports("consteval int loops_forever() { return loops_forever(); }\n"
                                                   "int main() { return loops_forever(); }\n");
    auto result = scpp::fold_immediate_calls(program);
    expect(!result.has_value(), case_name + ": expected runaway consteval recursion to report an error");
    if (result.has_value()) return;

    // This error has already been moved into std::unexpected and back out
    // again by the time it arrives here, so it has been through the copy
    // constructor under test.
    const scpp::ConstexprError& error = result.error();
    const std::string message = error.what();
    expect(!message.empty(), case_name + ": copied ConstexprError should retain its message");
    // ConstexprError's constructor prefixes the message with "line:column: ",
    // so a surviving message proves the base string was rebuilt, and a
    // matching loc proves the copy carried the location member too.
    expect(message.find(':') != std::string::npos,
           case_name + ": copied message should retain its 'line:column: ' prefix, got: " + message);
    expect(error.loc.line > 0, case_name + ": copied ConstexprError should retain a real source line");
    const std::string expected_prefix = std::to_string(error.loc.line) + ":" + std::to_string(error.loc.column) + ": ";
    expect(message.rfind(expected_prefix, 0) == 0,
           case_name + ": copied message prefix should agree with the copied loc; expected '" + expected_prefix +
               "' at the start of: " + message);

    // An explicit second copy, exercising the constructor directly rather
    // than only through std::expected's internals.
    scpp::ConstexprError copied = error;
    expect(std::string(copied.what()) == message, case_name + ": an explicit copy should preserve what()");
    expect(copied.loc.line == error.loc.line && copied.loc.column == error.loc.column,
           case_name + ": an explicit copy should preserve loc");
}

// PR #417: replacing CellData's std::variant with an explicit tagged class
// exposed two latent null dereferences. Both sites checked that the cell
// held a PointerValue but then dereferenced `pointer->storage` without
// checking it -- and a PointerValue's storage is genuinely null for a
// default-initialized `const char*`. Under std::variant this was
// `std::get_if<ArrayValue>(&pointer->storage->data)`, a hard crash; the
// tagged rewrite added the missing `storage` null-check at both sites.
//
// The first case below segfaults the compiler outright without the fix
// (verified: rc=139 against the pre-#417 constexpression.cppm), so it is a
// true regression test rather than a restatement of current behavior.
void run_constexpr_null_pointer_storage_tests() {
    {
        // A `const char*` struct field left default-initialized evaluates
        // to a PointerValue whose storage is null. Returning it from a
        // consteval function drives rewrite_expr_as_constant down its
        // const-char-pointer branch with that null storage.
        std::string case_name = "consteval_null_char_pointer_result_is_reported_not_crashed";
        cases_run++;
        auto ir = try_generate_ir("struct Holder { const char* text{}; };\n"
                                  "consteval const char* get() { Holder h{}; return h.text; }\n"
                                  "int main() { const char* q = get(); return 0; }\n");
        expect(!ir.has_value(), case_name + ": expected a diagnostic rather than a crash or success");
        if (ir.has_value()) return;
        expect(ir.error().kind == "ConstexprError",
               case_name + ": expected a ConstexprError, got " + ir.error().kind);
        // The point of the assertion is that we got *any* orderly
        // diagnostic back at all -- without the storage null-check this
        // call never returns.
        expect(!ir.error().message.empty(), case_name + ": expected a non-empty diagnostic message");
    }

    {
        // The null-check must not have made the ordinary string-literal
        // path stricter: a real `const char*` constexpr result still has
        // non-null storage backed by an ArrayValue and must still fold.
        std::string case_name = "consteval_string_literal_pointer_result_still_folds";
        cases_run++;
        auto ir = try_generate_ir("consteval const char* greeting() { return \"hi\"; }\n"
                                  "int main() { const char* g = greeting(); return 0; }\n");
        expect(ir.has_value(), case_name + ": expected a real string-literal constexpr pointer to still fold, got " +
                                   (ir.has_value() ? std::string() : ir.error().message));
    }
}

// PR #417: CellData is internal to the scpp.constexpression module (it
// lives in the module's non-exported `namespace scpp`), so its accessors
// cannot be called directly from this test binary. What *is* observable
// through the public API is the invariant those accessors exist to
// protect: `kind` and payload never disagree, because every write goes
// through a set_*() method that first releases the previous payload.
//
// Each case below folds a constexpr value of a different CellKind and
// checks the result comes back as that kind's value -- which is exactly a
// tag/payload agreement check. Under the old std::variant a tag mismatch
// aborted via std::get; under the tagged class it would instead surface
// as a silently stale payload, so these guard the failure mode the
// rewrite introduced the risk of.
void run_constexpr_cell_data_kind_tests() {
    struct KindCase {
        const char* name;
        const char* source;
    };
    const KindCase cases[] = {
        {"integer", "consteval int f() { return 41 + 1; }\nint main() { return f() - 42; }\n"},
        {"bool", "consteval bool f() { return 1 == 1; }\nint main() { if (f()) { return 0; } return 1; }\n"},
        {"double", "consteval double f() { return 0.5 + 0.25; }\nint main() { double d = f(); return 0; }\n"},
        {"char", "consteval char f() { return 'x'; }\nint main() { char c = f(); return 0; }\n"},
        {"string_literal_pointer", "consteval const char* f() { return \"ok\"; }\nint main() { const char* s = f(); return 0; }\n"},
        // CellKind::Array is covered transitively by the case above: a
        // string literal's backing storage *is* an ArrayValue cell, which
        // the const-char-pointer branch walks element by element. scpp has
        // no consteval-local array form to exercise it more directly.
        {"object_field",
         "struct Point { int x = 1; int y = 2; };\nconsteval int f() { Point p{}; return p.y; }\n"
         "int main() { return f() - 2; }\n"},
    };
    for (const KindCase& kind_case : cases) {
        std::string case_name = std::string("cell_data_round_trips_") + kind_case.name;
        cases_run++;
        auto ir = try_generate_ir(kind_case.source);
        expect(ir.has_value(), case_name + ": expected constexpr folding to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
    }
}

// PR #419: ObjectValue::fields changed from an
// std::unordered_map<std::string, std::shared_ptr<Cell>> to an ordered
// std::vector<ObjectField>. That is not purely mechanical -- it changes
// two observable properties of snapshot_constexpr_value's output, and
// both are load-bearing:
//
//  1. Ordering. The map iterated in *hash* order, so ConstexprValue::
//     object_fields came out in an order unrelated to the struct's
//     declaration order. The vector preserves insertion order, which is
//     the order make_default_cell/collect_class_fields create fields in,
//     i.e. declaration order. codegen's only consumer
//     (codegen/expressions.cppm's find_if over object_fields) looks fields
//     up by name, so this is a latent-hazard fix rather than an output
//     change -- but "the snapshot is deterministic and in declaration
//     order" is exactly the kind of property that silently regresses if
//     someone later swaps the container back.
//
//  2. Duplicate-name collapsing. collect_class_fields flattens base and
//     derived fields into one list without de-duplicating, so a derived
//     class redeclaring a base field's name yields that name twice.
//     unordered_map::emplace silently kept the first; ObjectValue::
//     add_field has to reproduce that explicitly, and a plain
//     emplace_back would not. The add_field comment calls this out as
//     load-bearing, so it gets a test rather than only a comment.
//
// Both cases below drive the real public API (evaluate_immediate_expr) on
// a consteval call that yields an object, then inspect the snapshot.
const scpp::Expr* find_first_var_decl_init(const scpp::Stmt& stmt) {
    if (stmt.kind == scpp::StmtKind::VarDecl) return stmt.init.get();
    for (const scpp::StmtPtr& nested : stmt.statements) {
        if (nested == nullptr) continue;
        if (const scpp::Expr* found = find_first_var_decl_init(*nested)) return found;
    }
    return nullptr;
}

const scpp::Stmt* find_var_decl_by_name(const scpp::Stmt& stmt, const std::string& var_name) {
    if (stmt.kind == scpp::StmtKind::VarDecl && stmt.var_name == var_name) return &stmt;
    for (const scpp::StmtPtr& nested : stmt.statements) {
        if (nested == nullptr) continue;
        if (const scpp::Stmt* found = find_var_decl_by_name(*nested, var_name)) return found;
    }
    return nullptr;
}

const scpp::Expr* find_initializer_in_function(const scpp::Program& program, const std::string& function_name) {
    for (const scpp::Function& fn : program.functions) {
        if (fn.name != function_name || fn.body == nullptr) continue;
        return find_first_var_decl_init(*fn.body);
    }
    return nullptr;
}

void run_constexpr_object_field_order_tests() {
    {
        // Declaration order is deliberately not alphabetical and the names
        // are deliberately unrelated, so that the sequence below is a real
        // assertion about insertion order rather than something a hashed
        // container could reproduce by luck.
        std::string case_name = "object_fields_snapshot_in_declaration_order";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "struct Wide {\n"
            "    int zulu = 1;\n"
            "    int alpha = 2;\n"
            "    int mike = 3;\n"
            "    int bravo = 4;\n"
            "    int yankee = 5;\n"
            "    int charlie = 6;\n"
            "    int november = 7;\n"
            "    int delta = 8;\n"
            "};\n"
            "consteval Wide make_wide() { Wide w{}; return w; }\n"
            "int main() { Wide w = make_wide(); return 0; }\n");
        const scpp::Expr* init = find_initializer_in_function(program, "main");
        expect(init != nullptr, case_name + ": expected to find the initializer of `Wide w = make_wide();`");
        if (init == nullptr) return;

        auto snapshot = scpp::evaluate_immediate_expr(program, *init);
        expect(snapshot.has_value(), case_name + ": expected the consteval call to evaluate, got " +
                                         (snapshot.has_value() ? std::string() : snapshot.error().what()));
        if (!snapshot.has_value()) return;
        expect(snapshot.value().kind == scpp::ConstexprValueKind::Object,
               case_name + ": expected an Object-kinded snapshot");

        const std::vector<std::string> expected_order = {"zulu",   "alpha",    "mike",  "bravo",
                                                         "yankee", "charlie", "november", "delta"};
        std::vector<std::string> actual_order;
        for (const auto& field : snapshot.value().object_fields) actual_order.push_back(field.first);

        expect(actual_order == expected_order,
               case_name + ": object_fields should follow struct declaration order; expected [" +
                   join_names(expected_order) + "] but got [" + join_names(actual_order) + "]");
    }

    {
        // A derived class redeclaring a base field name: collect_class_fields
        // emits `tag` twice, and ObjectValue::add_field must keep only the
        // first, exactly as unordered_map::emplace used to.
        std::string case_name = "object_fields_collapse_shadowed_base_field_names";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "class Base {\n"
            "public:\n"
            "    int tag = 1;\n"
            "    int only_base = 2;\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    int tag = 3;\n"
            "    int only_derived = 4;\n"
            "};\n"
            "consteval Derived make_derived() { Derived d{}; return d; }\n"
            "int main() { Derived d = make_derived(); return 0; }\n");
        const scpp::Expr* init = find_initializer_in_function(program, "main");
        expect(init != nullptr, case_name + ": expected to find the initializer of `Derived d = make_derived();`");
        if (init == nullptr) return;

        auto snapshot = scpp::evaluate_immediate_expr(program, *init);
        expect(snapshot.has_value(), case_name + ": expected the consteval call to evaluate, got " +
                                         (snapshot.has_value() ? std::string() : snapshot.error().what()));
        if (!snapshot.has_value()) return;

        std::vector<std::string> actual_order;
        for (const auto& field : snapshot.value().object_fields) actual_order.push_back(field.first);
        const std::size_t tag_count =
            static_cast<std::size_t>(std::count(actual_order.begin(), actual_order.end(), std::string("tag")));
        expect(tag_count == 1, case_name + ": a shadowed base field name should appear exactly once, got " +
                                   std::to_string(tag_count) + " in [" + join_names(actual_order) + "]");
    }
}

void run_constexpr_string_literal_byte_tests() {
    // The string-literal cell builder used to iterate `for (unsigned char ch :
    // expr.name)`. scpp cannot spell that loop, so it is now an index loop that
    // casts through std::uint8_t. Bytes >= 0x80 must therefore still widen as
    // unsigned; a plain `static_cast<std::int64_t>(expr.name.at(i))` would
    // sign-extend `char` and turn 0xE2 into -30.
    struct ByteCase {
        std::string case_name;
        std::vector<int> bytes;
        std::int64_t index;
        std::int64_t expected;
    };
    // scpp's lexer supports only \n \t \r \\ \' \" \0, so the high bytes are
    // embedded literally rather than as \x escapes.
    const std::vector<ByteCase> byte_cases = {
        {"string_literal_high_byte_stays_unsigned_first", {0xE2, 0x82, 0xAC}, 0, 226},
        {"string_literal_high_byte_stays_unsigned_middle", {0xE2, 0x82, 0xAC}, 1, 130},
        {"string_literal_high_byte_stays_unsigned_last", {0xE2, 0x82, 0xAC}, 2, 172},
        {"string_literal_ascii_byte_unchanged", {0x41, 0x7F}, 1, 127},
        {"string_literal_max_byte_stays_unsigned", {0xFF}, 0, 255},
        {"string_literal_nul_terminator_present", {0xFF}, 1, 0},
    };

    for (const ByteCase& byte_case : byte_cases) {
        cases_run++;
        std::string literal;
        for (int byte : byte_case.bytes) literal.push_back(static_cast<char>(byte));
        scpp::Program program = parse_with_std_imports(
            "consteval int byte_at() { const char* s = \"" + literal + "\"; return s[" +
            std::to_string(byte_case.index) +
            "]; }\n"
            "int main() { int v = byte_at(); return 0; }\n");
        const scpp::Expr* init = find_initializer_in_function(program, "main");
        expect(init != nullptr, byte_case.case_name + ": expected to find the initializer of `int v = byte_at();`");
        if (init == nullptr) continue;

        auto snapshot = scpp::evaluate_immediate_expr(program, *init);
        expect(snapshot.has_value(), byte_case.case_name + ": expected the consteval call to evaluate, got " +
                                         (snapshot.has_value() ? std::string() : snapshot.error().what()));
        if (!snapshot.has_value()) continue;
        expect(snapshot.value().kind == scpp::ConstexprValueKind::Integer,
               byte_case.case_name + ": expected an Integer-kinded snapshot");
        expect(snapshot.value().int_value == byte_case.expected,
               byte_case.case_name + ": expected byte " + std::to_string(byte_case.expected) + " but got " +
                   std::to_string(snapshot.value().int_value));
    }
}

// Every diagnostic in constexpression.cppm is now assembled with `+=` onto a
// named std::string rather than a chain of `+`, because scpp has no
// `operator+` on strings. Message text is user-visible, so the point of these
// cases is that the reassembly kept every message byte-identical -- including
// the "line:column: " prefix, which had to move into a free function since a
// base-class mem-initializer cannot bind a temporary.
void run_constexpr_diagnostic_text_tests() {
    struct DiagnosticCase {
        std::string case_name;
        std::string source;
        std::string expected_message;
    };
    const std::vector<DiagnosticCase> diagnostic_cases = {
        {"diagnostic_unknown_constexpr_field",
         "struct Point { int x = 1; };\n"
         "consteval int bad() { Point p{}; return p.missing; }\n"
         "int main() { return bad(); }\n",
         "unknown constexpr field 'missing'"},
        {"diagnostic_no_overload_matches_immediate_call",
         "consteval int f(int a) { return a; }\n"
         "consteval int bad() { return f(1, 2); }\n"
         "int main() { return bad(); }\n",
         "no constexpr/consteval overload of 'f' matches this immediate call"},
        {"diagnostic_no_method_overload_matches_immediate_call",
         "class C {\n  public:\n    consteval int m(int a) { return a; }\n};\n"
         "consteval int bad() { C c{}; return c.m(1, 2); }\n"
         "int main() { return bad(); }\n",
         "no constexpr/consteval overload of method 'm' matches this immediate call"},
        {"diagnostic_no_constructor_matches_for_type",
         "class Holder {\n"
         "  public:\n"
         "    consteval Holder(int a, int b) { this->v = a + b; }\n"
         "    int v = 0;\n"
         "};\n"
         "consteval int bad() { Holder h{1}; return h.v; }\n"
         "int main() { return bad(); }\n",
         "no constexpr/consteval constructor matches for type 'Holder'"},
        // The two cases below are the ones whose message tail is appended from
        // a parameter rather than a literal: 'sizeof' arrives as a `const
        // char*` and "variable 'x'" as a `const std::string&`, so together
        // they pin both `+=` overloads used to splice a caller-supplied
        // fragment into a diagnostic.
        {"diagnostic_incomplete_type_sizeof",
         "struct S {\n"
         "    alignas(sizeof(S)) int a = 0;\n"
         "};\n",
         "cannot apply 'sizeof' to 'S': it is still an incomplete type at this point"},
        {"diagnostic_alignas_less_strict_than_natural",
         "consteval int bad() {\n"
         "    alignas(1) int x = 0;\n"
         "    return x;\n"
         "}\n"
         "int main() { return bad(); }\n",
         "'alignas' requests alignment 1, which is less strict than the natural alignment 4 of variable 'x'"},
    };

    for (const DiagnosticCase& diagnostic_case : diagnostic_cases) {
        cases_run++;
        auto parsed = try_parse_with_std_imports(diagnostic_case.source);
        expect(parsed.has_value(), diagnostic_case.case_name + ": expected the source to parse, got " +
                                       (parsed.has_value() ? std::string() : parsed.error().what()));
        if (!parsed.has_value()) continue;
        scpp::Program program = std::move(parsed).value();
        auto result = scpp::fold_immediate_calls(program);
        expect(!result.has_value(), diagnostic_case.case_name + ": expected an error");
        if (result.has_value()) continue;

        const scpp::ConstexprError& error = result.error();
        const std::string what = error.what();
        // ConstexprError prefixes "line:column: "; assert both halves exactly.
        std::string expected_prefix;
        expected_prefix += std::to_string(error.loc.line);
        expected_prefix += ":";
        expected_prefix += std::to_string(error.loc.column);
        expected_prefix += ": ";
        expect(what == expected_prefix + diagnostic_case.expected_message,
               diagnostic_case.case_name + ": expected '" + expected_prefix + diagnostic_case.expected_message +
                   "' but got '" + what + "'");
    }

    {
        // The step-budget tail is built from the same `const char*` parameter
        // path as the 'sizeof' case above, but reached through the evaluator's
        // recursion guard rather than a layout check.
        std::string case_name = "diagnostic_step_budget_exhausted";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "consteval int count(int n) { if (n <= 0) { return 0; } return count(n - 1) + 1; }\n"
            "int main() { return count(50); }\n");
        scpp::ConstexprLimits limits{};
        limits.max_steps = 32;
        auto result = scpp::fold_immediate_calls(program, limits);
        expect(!result.has_value(), case_name + ": expected the step budget to be exceeded");
        if (result.has_value()) return;
        const std::string what = result.error().what();
        const std::string expected_fragment = "constexpr evaluation exceeded step budget while ";
        expect(what.find(expected_fragment) != std::string::npos,
               case_name + ": expected the message to contain '" + expected_fragment + "' but got '" + what + "'");
        expect(what.size() > what.find(expected_fragment) + expected_fragment.size(),
               case_name + ": expected a non-empty 'while ...' tail in '" + what + "'");
    }
}

} // namespace


// ConstexprEngine indexes the program's classes, structs, globals and
// function overloads by name. Those indexes used to hold raw pointers taken
// with `&def` inside a range-for; they now hold positions into the matching
// `program_` vector. A pointer could only ever be right or dangling, but an
// index can silently be *off*, which would quietly resolve a name to a
// neighbouring definition. Each case below therefore declares several
// same-shaped definitions with distinguishable payloads and asserts that a
// name resolves to its own definition, not to the one beside it.
void run_constexpr_name_index_tests() {
    struct IndexCase {
        std::string case_name;
        std::string source;
        std::int64_t expected_value;
    };
    const std::vector<IndexCase> index_cases = {
        // structs_by_name_: `Third` is neither first nor last in
        // program_.structs, so an off-by-one either way is visible.
        {"struct_defaults_resolve_to_their_own_definition",
         "struct First { int v = 11; };\n"
         "struct Second { int v = 22; };\n"
         "struct Third { int v = 33; };\n"
         "struct Fourth { int v = 44; };\n"
         "consteval int probe() { Third t{}; return t.v; }\n"
         "int main() { int r = probe(); return r; }\n",
         33},
        // classes_by_name_, including the base-class walk in
        // collect_class_fields: BaseB is the second of three bases.
        {"class_base_fields_resolve_to_their_own_definition",
         "class BaseA {\n  public:\n    int a = 1;\n};\n"
         "class BaseB {\n  public:\n    int a = 7;\n};\n"
         "class BaseC {\n  public:\n    int a = 9;\n};\n"
         "class Derived : public BaseB {\n  public:\n    int b = 100;\n};\n"
         "consteval int probe() { Derived d{}; return d.a + d.b; }\n"
         "int main() { int r = probe(); return r; }\n",
         107},
        // globals_by_name_: `g_third` sits in the middle of program_.globals.
        {"global_constants_resolve_to_their_own_definition",
         "constexpr int g_first = 5;\n"
         "constexpr int g_second = 6;\n"
         "constexpr int g_third = 7;\n"
         "constexpr int g_fourth = 8;\n"
         "consteval int probe() { return g_third; }\n"
         "int main() { int r = probe(); return r; }\n",
         7},
        // functions_by_name_ maps one name to a vector of overload
        // positions, so it is the only index built with an insert-or-append
        // step. Calling both overloads means the case fails if the append
        // branch drops the second position rather than merely misplacing it.
        {"function_overloads_resolve_to_their_own_definition",
         "consteval int pick(int a) { return 1000 + a; }\n"
         "consteval int pick(bool b) { return b ? 1 : 2; }\n"
         "consteval int probe() { return pick(5) + pick(true); }\n"
         "int main() { int r = probe(); return r; }\n",
         1006},
    };

    for (const IndexCase& index_case : index_cases) {
        cases_run++;
        scpp::Program program = parse_with_std_imports(index_case.source);
        const scpp::Expr* init = find_initializer_in_function(program, "probe");
        static_cast<void>(init);
        auto folded = scpp::fold_immediate_calls(program);
        expect(folded.has_value(), index_case.case_name + ": expected folding to succeed, got " +
                                       (folded.has_value() ? std::string() : folded.error().what()));
        if (!folded.has_value()) continue;

        const scpp::Expr* main_init = find_initializer_in_function(program, "main");
        expect(main_init != nullptr, index_case.case_name + ": expected to find main's initializer");
        if (main_init == nullptr) continue;

        auto value = scpp::evaluate_immediate_expr(program, *main_init);
        expect(value.has_value(), index_case.case_name + ": expected the consteval call to evaluate, got " +
                                      (value.has_value() ? std::string() : value.error().what()));
        if (!value.has_value()) continue;
        expect(value.value().kind == scpp::ConstexprValueKind::Integer,
               index_case.case_name + ": expected an Integer-kinded snapshot");
        expect(value.value().int_value == index_case.expected_value,
               index_case.case_name + ": expected " + std::to_string(index_case.expected_value) + " but got " +
                   std::to_string(value.value().int_value));
    }
}


// constexpression.cppm deliberately discards three [[nodiscard]] results,
// each replacing an explicit `catch (const ConstexprError&) {}` from the
// pre-std::expected code: a local `const` whose initializer is not a
// constant expression simply does not become one, which is not an error.
// That is easy to mistake for a dropped error and "fix" into a propagation,
// which would reject ordinary runtime code, so pin that they stay swallowed.
void run_constexpr_best_effort_const_local_tests() {
    struct ToleratedCase {
        std::string case_name;
        std::string source;
    };
    const std::vector<ToleratedCase> tolerated_cases = {
        // validate_constexpr_stmt_tree's VarDecl `is_const` branch.
        {"const_local_with_runtime_initializer_is_tolerated",
         "int runtime(int n) {\n"
         "    const int c = n + 1;\n"
         "    return c;\n"
         "}\n"
         "int main() { return runtime(2); }\n"},
        // bind_local_constant_for_array_bounds' own `is_const` branch: the
        // same statement shape, reached while resolving a later local array
        // bound in the same function.
        {"const_local_with_runtime_initializer_beside_array_bound_is_tolerated",
         "int runtime(int n) {\n"
         "    const int c = n + 1;\n"
         "    int arr[4];\n"
         "    return arr[0] + c;\n"
         "}\n"
         "int main() { return runtime(2); }\n"},
        // validate_constexpr_stmt_tree's `is_constexpr` branch: the value is a
        // perfectly valid constant, but an object result cannot be lowered
        // back into a source-form AST literal, so the rewrite is skipped and
        // the original initializer is kept.
        {"constexpr_object_local_that_cannot_be_lowered_is_tolerated",
         "struct Point { int x{}; int y{}; };\n"
         "constexpr Point make_point() { Point p{}; p.x = 1; p.y = 2; return p; }\n"
         "int runtime() {\n"
         "    constexpr Point p = make_point();\n"
         "    return p.x;\n"
         "}\n"
         "int main() { return runtime(); }\n"},
    };

    for (const ToleratedCase& tolerated_case : tolerated_cases) {
        cases_run++;
        scpp::Program program = parse_with_std_imports(tolerated_case.source);
        auto result = scpp::fold_immediate_calls(program);
        expect(result.has_value(),
               tolerated_case.case_name +
                   ": a const local whose initializer is not a constant expression must stay tolerated, got " +
                   (result.has_value() ? std::string() : result.error().what()));
    }
}




// resolve_alignment_specs reports "no explicit alignment needed" as zero, and
// only reports a real alignment when the request is *stricter* than the type's
// natural alignment. Nothing else pins that: dropping the comparison entirely
// (always reporting the requested value) leaves codegen_test and every alignas
// blackbox case passing, because over-aligning to the natural alignment is a
// no-op at runtime. So check the resolved value directly.
void run_constexpr_resolved_alignment_tests() {
    struct AlignmentCase {
        std::string case_name;
        std::string alignas_argument;
        std::uint64_t expected_resolved;
    };
    // `int` has a natural alignment of 4, so 4 is not stricter and must
    // resolve to 0, while 16 is stricter and must resolve to 16.
    const std::vector<AlignmentCase> alignment_cases = {
        {"alignas_stricter_than_natural_resolves_to_the_request", "16", 16},
        {"alignas_equal_to_natural_resolves_to_zero", "4", 0},
    };

    for (const AlignmentCase& alignment_case : alignment_cases) {
        cases_run++;
        std::string source{};
        source += "int main() {\n";
        source += "    alignas(";
        source += alignment_case.alignas_argument;
        source += ") int aligned_local = 0;\n";
        source += "    return aligned_local;\n";
        source += "}\n";

        scpp::Program program = parse_with_std_imports(source);
        auto fold_result = scpp::fold_immediate_calls(program);
        expect(fold_result.has_value(),
               alignment_case.case_name + ": expected the program to fold, got " +
                   (fold_result.has_value() ? std::string() : fold_result.error().what()));
        if (!fold_result.has_value()) continue;

        const scpp::Stmt* declaration = nullptr;
        for (const scpp::Function& fn : program.functions) {
            if (fn.name != "main" || fn.body == nullptr) continue;
            declaration = find_var_decl_by_name(*fn.body, "aligned_local");
        }
        expect(declaration != nullptr, alignment_case.case_name + ": expected to find `aligned_local`");
        if (declaration == nullptr) continue;

        expect(declaration->resolved_alignment == alignment_case.expected_resolved,
               alignment_case.case_name + ": expected resolved_alignment " +
                   std::to_string(alignment_case.expected_resolved) + " but got " +
                   std::to_string(declaration->resolved_alignment));
    }
}

// Compiler bug #8: an array of class type had no object lifetime at all.
// Every lifetime decision in codegen was spelled as a by-*name* question
// about a class ("does `Tracked` have a destructor", "run `Tracked`'s
// constructor here"), and each call site guarded it with a hand-written
// `type.kind == TypeKind::Named` test. A `Type` whose kind is Array
// carries no class name, so every one of those guards silently answered
// "no lifetime" for it: `Tracked arr[3]{};` ran zero constructors and zero
// destructors, and an array of a polymorphic class was left with null
// vtable pointers, so a virtual call through an element segfaulted.
//
// The by-name entry points now have by-type counterparts
// (type_has_destructor / emit_storage_destruction /
// codegen_destroy_storage_unless_moved / type_needs_nontrivial_default_init)
// that dispatch Named to the old logic unchanged and dispatch Array by
// looping over its elements and *recursing into that same logic*, so an
// element is initialized and destroyed exactly the way a standalone
// object of the element type is. Construction runs front to back,
// destruction back to front.
void run_array_element_lifetime_tests() {
    const std::string tracked_class =
        "class Tracked {\n"
        "  public:\n"
        "    Tracked() { this->tag = 1; return; }\n"
        "    virtual ~Tracked() { this->tag = 0; return; }\n"
        "    int tag = 0;\n"
        "};\n";

    {
        // The reported shape. One call site each, inside a counted loop
        // over the elements -- a loop rather than three unrolled calls so
        // that the IR for a large array stays bounded.
        std::string case_name = "array_of_class_type_is_constructed_and_destroyed";
        cases_run++;
        auto ir = try_generate_ir(tracked_class + "int main() {\n"
                                                  "    Tracked arr[3]{};\n"
                                                  "    return arr[0].tag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(!main_ir.empty(), case_name + ": expected a definition of `main` in the module IR");
            std::size_t constructions = count_occurrences(main_ir, "call void @Tracked_new(");
            expect(constructions == 1, case_name + ": expected exactly one element-constructor call site, got " +
                                           std::to_string(constructions));
            std::size_t destructions = count_occurrences(main_ir, "call void @Tracked_delete(");
            expect(destructions == 1, case_name + ": expected exactly one element-destructor call site, got " +
                                          std::to_string(destructions));
        }
    }

    {
        // The loop has to actually cover every element, and destruction
        // has to run back to front: construction counts up from 0 and
        // stops at the bound, destruction counts down and stops at 0.
        std::string case_name = "element_loops_cover_every_element_and_destroy_in_reverse";
        cases_run++;
        auto ir = try_generate_ir(tracked_class + "int main() {\n"
                                                  "    Tracked arr[3]{};\n"
                                                  "    return arr[0].tag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(main_ir.find("arrayelem.i = phi i64 [ 0,") != std::string::npos,
                   case_name + ": expected the construction loop to start at element 0");
            expect(main_ir.find("= add i64 %arrayelem.i") != std::string::npos,
                   case_name + ": expected the construction loop to walk forwards");
            expect(main_ir.find("icmp eq i64 %arrayelem.next, 3") != std::string::npos,
                   case_name + ": expected the construction loop to stop after the last element");
            expect(main_ir.find("phi i64 [ 2,") != std::string::npos,
                   case_name + ": expected the destruction loop to start at the last element");
            expect(main_ir.find("= sub i64 %arrayelem.i") != std::string::npos,
                   case_name + ": expected the destruction loop to walk backwards");
        }
    }

    {
        // A zero-cost path must stay zero-cost: an array of a scalar type
        // has no per-element work to do, so it keeps the single memset.
        std::string case_name = "array_of_scalar_type_still_uses_a_single_zero_fill";
        cases_run++;
        auto ir = try_generate_ir("int main() {\n"
                                  "    int nums[4]{};\n"
                                  "    return nums[0];\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed");
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(main_ir.find("arrayelem.body") == std::string::npos,
                   case_name + ": expected no per-element loop for an array of scalars");
            expect(main_ir.find("llvm.memset") != std::string::npos,
                   case_name + ": expected the array to still be zero-filled");
        }
    }

    {
        // Nested arrays are just arrays whose element type is an array,
        // so the same recursion has to reach the leaf elements.
        std::string case_name = "nested_array_elements_are_constructed_and_destroyed";
        cases_run++;
        auto ir = try_generate_ir(tracked_class + "int main() {\n"
                                                  "    Tracked grid[2][2]{};\n"
                                                  "    return grid[0][0].tag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(count_occurrences(main_ir, "call void @Tracked_new(") == 1,
                   case_name + ": expected the nested element constructor to be called");
            expect(count_occurrences(main_ir, "call void @Tracked_delete(") == 1,
                   case_name + ": expected the nested element destructor to be called");
            expect(count_occurrences(main_ir, "arrayelem.body") >= 4,
                   case_name + ": expected an outer and an inner element loop for both construction and destruction");
        }
    }

    {
        // An array of a polymorphic class used to be left with null vtable
        // pointers, because the whole-array memset is not the element's
        // value-initialization. A virtual call through an element then
        // dispatched through null and crashed at runtime.
        std::string case_name = "array_of_polymorphic_class_gets_element_vtable_pointers";
        cases_run++;
        auto ir = try_generate_ir("class Base {\n"
                                  "  public:\n"
                                  "    virtual ~Base() = default;\n"
                                  "    virtual int who() { return 3; }\n"
                                  "};\n"
                                  "int main() {\n"
                                  "    Base arr[2]{};\n"
                                  "    return arr[0].who();\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(main_ir.find("store ptr @__scpp_vtable.Base") != std::string::npos,
                   case_name + ": expected each element's vtable pointer to be initialized");
        }
    }

    {
        // A class-typed array *member* is initialized by the same
        // initialize_storage path, and torn down by the defaulted
        // destructor's field walk -- both of which used to skip arrays.
        std::string case_name = "class_member_array_is_constructed_and_destroyed";
        cases_run++;
        auto ir = try_generate_ir(tracked_class + "class Holder {\n"
                                                  "  public:\n"
                                                  "    Holder() { return; }\n"
                                                  "    virtual ~Holder() = default;\n"
                                                  "    Tracked items[2]{};\n"
                                                  "};\n"
                                                  "int main() {\n"
                                                  "    Holder h{};\n"
                                                  "    return h.items[0].tag;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string ctor_ir = function_ir(ir.value(), "Holder_new");
            expect(!ctor_ir.empty(), case_name + ": expected a definition of `Holder_new`");
            expect(count_occurrences(ctor_ir, "call void @Tracked_new(") == 1,
                   case_name + ": expected the member array's elements to be constructed");
            std::string dtor_ir = function_ir(ir.value(), "Holder_delete");
            expect(!dtor_ir.empty(), case_name + ": expected a definition of `Holder_delete`");
            expect(count_occurrences(dtor_ir, "call void @Tracked_delete(") == 1,
                   case_name + ": expected the member array's elements to be destroyed");
        }
    }

    {
        // is_field_copy_constructible already looked *through* an array to
        // its element type when deciding a field was copyable, but
        // emission then bitwise-copied the whole array -- so the very copy
        // constructor that check approved never ran. Same for assignment.
        std::string case_name = "class_member_array_is_copied_element_by_element";
        cases_run++;
        auto ir = try_generate_ir("class Item {\n"
                                  "  public:\n"
                                  "    Item() { this->v = 1; return; }\n"
                                  "    Item(const Item& other) { this->v = other.v + 1; return; }\n"
                                  "    Item& operator=(const Item& other) { this->v = other.v + 2; return *this; }\n"
                                  "    virtual ~Item() = default;\n"
                                  "    int v = 0;\n"
                                  "};\n"
                                  "class Holder {\n"
                                  "  public:\n"
                                  "    Holder() { return; }\n"
                                  "    Holder(const Holder& other) = default;\n"
                                  "    Holder& operator=(const Holder& other) = default;\n"
                                  "    virtual ~Holder() = default;\n"
                                  "    Item items[2]{};\n"
                                  "};\n"
                                  "int main() {\n"
                                  "    Holder a{};\n"
                                  "    Holder b{a};\n"
                                  "    b = a;\n"
                                  "    return b.items[0].v;\n"
                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string copy_ctor_ir = function_ir(ir.value(), "Holder_new.Holder_ref.Holder_cref");
            expect(!copy_ctor_ir.empty(), case_name + ": expected a definition of the defaulted copy constructor");
            expect(count_occurrences(copy_ctor_ir, "call void @Item_new.Item_ref.Item_cref(") == 1,
                   case_name + ": expected the member array to be copy-constructed element by element rather than "
                               "bitwise-copied, got " +
                       std::to_string(count_occurrences(copy_ctor_ir, "call void @Item_new.Item_ref.Item_cref(")) +
                       " element copy-constructor call site(s)");
            std::string copy_assign_ir = function_ir(ir.value(), "Holder_operator_assign");
            expect(!copy_assign_ir.empty(), case_name + ": expected a definition of the defaulted copy-assignment operator");
            expect(count_occurrences(copy_assign_ir, "@Item_operator_assign(") == 1,
                   case_name + ": expected the member array to be copy-assigned element by element, got " +
                       std::to_string(count_occurrences(copy_assign_ir, "@Item_operator_assign(")) +
                       " element copy-assignment call site(s)");
        }
    }

    {
        // Scope-exit cleanup reaches arrays through every exit path, not
        // just the fall-off-the-end one: an early return goes through
        // emit_scope_cleanup_to_depth rather than pop_scope, and both used
        // to carry their own copy of the `kind == Named` guard.
        std::string case_name = "array_elements_are_destroyed_on_an_early_return";
        cases_run++;
        auto ir = try_generate_ir(tracked_class + "int main() {\n"
                                                  "    Tracked arr[2]{};\n"
                                                  "    if (arr[0].tag == 1) {\n"
                                                  "        return 1;\n"
                                                  "    }\n"
                                                  "    return 0;\n"
                                                  "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string main_ir = function_ir(ir.value(), "main");
            expect(count_occurrences(main_ir, "call void @Tracked_delete(") == 2,
                   case_name + ": expected the array to be destroyed on both the early-return and the fall-through path, got " +
                       std::to_string(count_occurrences(main_ir, "call void @Tracked_delete(")) + " destructor call site(s)");
        }
    }
}

// Destruction has to be as symmetric as construction: every lowering of a
// class's destructor -- `= default` and hand-written body alike -- must
// destroy exactly that class's own class-typed members, once, after the
// body, and must leave the base's members to the base's own destructor.
void run_user_destructor_member_teardown_tests() {
    const std::string inner_class =
        "class Inner {\n"
        "  public:\n"
        "    Inner() { this->tag = 1; return; }\n"
        "    virtual ~Inner() { this->tag = 0; return; }\n"
        "    int tag = 0;\n"
        "};\n";

    {
        // The reported shape: with a hand-written body, `Inner_delete`
        // was never called from `Outer_delete` at all.
        std::string case_name = "user_written_destructor_calls_its_members_destructors";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Outer {\n"
                                                "  public:\n"
                                                "    Outer() { return; }\n"
                                                "    virtual ~Outer() { return; }\n"
                                                "    Inner a{};\n"
                                                "    Inner b{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Outer o{};\n"
                                                "    return o.a.tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string dtor_ir = function_ir(ir.value(), "Outer_delete");
            expect(!dtor_ir.empty(), case_name + ": expected a definition of `Outer_delete` in the module IR");
            std::size_t member_destructions = count_occurrences(dtor_ir, "call void @Inner_delete(");
            expect(member_destructions == 2,
                   case_name + ": expected both members destroyed from the user-written destructor, got " +
                       std::to_string(member_destructions));
        }
    }

    {
        // `~T() { }` and `~T() = default;` must emit the same member
        // teardown -- they now share emit_class_member_teardown.
        std::string case_name = "defaulted_and_user_written_destructors_emit_the_same_member_teardown";
        cases_run++;
        std::string body =
            "class Outer {\n"
            "  public:\n"
            "    Outer() { return; }\n"
            "    virtual ~Outer() { PLACEHOLDER }\n"
            "    Inner a{};\n"
            "};\n"
            "int main() {\n"
            "    Outer o{};\n"
            "    return o.a.tag;\n"
            "}\n";
        std::string user_source = body;
        user_source.replace(user_source.find("PLACEHOLDER"), std::string("PLACEHOLDER").size(), "return;");
        std::string defaulted_source = body;
        defaulted_source.replace(defaulted_source.find("virtual ~Outer() { PLACEHOLDER }"),
                                 std::string("virtual ~Outer() { PLACEHOLDER }").size(), "virtual ~Outer() = default;");
        auto user_ir = try_generate_ir(inner_class + user_source);
        auto defaulted_ir = try_generate_ir(inner_class + defaulted_source);
        expect(user_ir.has_value() && defaulted_ir.has_value(), case_name + ": expected both programs to compile");
        if (user_ir.has_value() && defaulted_ir.has_value()) {
            std::size_t user_count = count_occurrences(function_ir(user_ir.value(), "Outer_delete"), "call void @Inner_delete(");
            std::size_t defaulted_count =
                count_occurrences(function_ir(defaulted_ir.value(), "Outer_delete"), "call void @Inner_delete(");
            expect(user_count == defaulted_count && user_count == 1,
                   case_name + ": expected one member destructor call from each spelling, got user=" +
                       std::to_string(user_count) + " defaulted=" + std::to_string(defaulted_count));
        }
    }

    {
        // A derived class's StructInfo carries the base's fields
        // flattened in front of its own; walking all of them destroyed
        // every inherited member a second time, since the base's own
        // destructor already handles them.
        std::string case_name = "derived_destructor_does_not_destroy_base_members";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Base {\n"
                                                "  public:\n"
                                                "    Base() { return; }\n"
                                                "    virtual ~Base() { return; }\n"
                                                "    Inner bm{};\n"
                                                "};\n"
                                                "class Derived : public Base {\n"
                                                "  public:\n"
                                                "    Derived() { return; }\n"
                                                "    ~Derived() override { return; }\n"
                                                "    Inner dm{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Derived d{};\n"
                                                "    return d.dm.tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::size_t derived_count =
                count_occurrences(function_ir(ir.value(), "Derived_delete"), "call void @Inner_delete(");
            expect(derived_count == 1, case_name + ": expected `Derived_delete` to destroy only its own member, got " +
                                           std::to_string(derived_count));
            std::size_t base_count = count_occurrences(function_ir(ir.value(), "Base_delete"), "call void @Inner_delete(");
            expect(base_count == 1, case_name + ": expected `Base_delete` to destroy its own member, got " +
                                        std::to_string(base_count));
        }
    }

    {
        // Same layout question, `= default` spelling: the double
        // destruction was a pre-existing defect of the defaulted path.
        std::string case_name = "defaulted_derived_destructor_does_not_destroy_base_members";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Base {\n"
                                                "  public:\n"
                                                "    Base() { return; }\n"
                                                "    virtual ~Base() = default;\n"
                                                "    Inner bm{};\n"
                                                "};\n"
                                                "class Derived : public Base {\n"
                                                "  public:\n"
                                                "    Derived() { return; }\n"
                                                "    ~Derived() override = default;\n"
                                                "    Inner dm{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Derived d{};\n"
                                                "    return d.dm.tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::size_t derived_count =
                count_occurrences(function_ir(ir.value(), "Derived_delete"), "call void @Inner_delete(");
            expect(derived_count == 1, case_name + ": expected one member destructor call in `Derived_delete`, got " +
                                           std::to_string(derived_count));
        }
    }

    {
        // Teardown is emitted per exit path, not once per function: two
        // `return;`s means two teardown sites, and none of them may be
        // missing.
        std::string case_name = "user_written_destructor_tears_down_on_every_return_path";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Outer {\n"
                                                "  public:\n"
                                                "    Outer() { return; }\n"
                                                "    virtual ~Outer() {\n"
                                                "        if (this->a.tag == 1) {\n"
                                                "            return;\n"
                                                "        }\n"
                                                "        return;\n"
                                                "    }\n"
                                                "    Inner a{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Outer o{};\n"
                                                "    return o.a.tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string dtor_ir = function_ir(ir.value(), "Outer_delete");
            std::size_t destructions = count_occurrences(dtor_ir, "call void @Inner_delete(");
            std::size_t returns = count_occurrences(dtor_ir, "ret void");
            expect(destructions == returns && destructions == 2,
                   case_name + ": expected one member teardown per return path, got " + std::to_string(destructions) +
                       " teardown(s) for " + std::to_string(returns) + " return(s)");
        }
    }

    {
        // The implicit `return;` synthesized when control falls off the
        // end of a void function is a return like any other and runs the
        // same epilogue.
        std::string case_name = "destructor_without_an_explicit_return_still_tears_down";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Outer {\n"
                                                "  public:\n"
                                                "    Outer() { return; }\n"
                                                "    virtual ~Outer() { }\n"
                                                "    Inner a{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Outer o{};\n"
                                                "    return o.a.tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::size_t destructions =
                count_occurrences(function_ir(ir.value(), "Outer_delete"), "call void @Inner_delete(");
            expect(destructions == 1, case_name + ": expected member teardown on the fall-off-the-end path, got " +
                                          std::to_string(destructions));
        }
    }

    {
        // An array member is a member: its element loop has to be emitted
        // from a hand-written destructor too.
        std::string case_name = "user_written_destructor_tears_down_array_typed_members";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "class Outer {\n"
                                                "  public:\n"
                                                "    Outer() { return; }\n"
                                                "    virtual ~Outer() { return; }\n"
                                                "    Inner items[3]{};\n"
                                                "};\n"
                                                "int main() {\n"
                                                "    Outer o{};\n"
                                                "    return o.items[0].tag;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::string dtor_ir = function_ir(ir.value(), "Outer_delete");
            std::size_t destructions = count_occurrences(dtor_ir, "call void @Inner_delete(");
            expect(destructions == 1, case_name + ": expected one element-destructor call site inside a loop, got " +
                                          std::to_string(destructions));
        }
    }

    {
        // Ordinary functions keep their epilogue exactly where it was:
        // this pins that adding the fall-off-the-end call did not double
        // up cleanup on the explicit-`return` path.
        std::string case_name = "explicit_return_path_emits_exactly_one_local_teardown";
        cases_run++;
        auto ir = try_generate_ir(inner_class + "void scope() {\n"
                                                "    Inner x{};\n"
                                                "    return;\n"
                                                "}\n"
                                                "int main() {\n"
                                                "    scope();\n"
                                                "    return 0;\n"
                                                "}\n");
        expect(ir.has_value(), case_name + ": expected IR generation to succeed, got " +
                                   (ir.has_value() ? std::string() : ir.error().kind + ": " + ir.error().message));
        if (ir.has_value()) {
            std::size_t destructions = count_occurrences(function_ir(ir.value(), "scope"), "call void @Inner_delete(");
            expect(destructions == 1, case_name + ": expected exactly one local teardown, got " +
                                          std::to_string(destructions));
        }
    }
}

int main() {
    run_test_case_files();
    test_generate_returns_engaged_expected_on_success();
    test_generate_returns_disengaged_expected_on_failure_without_throwing();
    run_constexpr_engine_direct_api_tests();
    run_switch_end_block_reachability_tests();
    run_local_shadowing_tests();
    run_virtual_base_initializer_frame_tests();
    run_value_initialized_temporary_constructor_tests();
    run_array_element_lifetime_tests();
    run_user_destructor_member_teardown_tests();
    run_return_type_checking_tests();

    run_constexpr_error_copy_tests();
    run_constexpr_null_pointer_storage_tests();
    run_constexpr_cell_data_kind_tests();
    run_constexpr_object_field_order_tests();
    run_constexpr_string_literal_byte_tests();
    run_constexpr_diagnostic_text_tests();
    run_constexpr_name_index_tests();
    run_constexpr_best_effort_const_local_tests();
    run_constexpr_resolved_alignment_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All codegen tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}

