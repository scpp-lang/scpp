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

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
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
};

std::expected<std::string, GenerateIrError> try_generate_ir(std::string_view source) {
    auto parse_result = try_parse_with_std_imports(source);
    if (!parse_result.has_value()) return std::unexpected(GenerateIrError{"ParseError", parse_result.error().what()});
    scpp::Program program = std::move(parse_result.value());
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value())
        return std::unexpected(GenerateIrError{"DataflowError", monomorphize_result.error().what()});
    // ch05 §9.4: resolves every array bound (and other constant-expression
    // context, e.g. `alignas`) before codegen ever reads a type's layout --
    // codegen itself never evaluates constant expressions, only the
    // already-resolved `Type::array_size`. Mirrors driver.cppm's own
    // pipeline ordering (monomorphize_generics -> fold_immediate_calls ->
    // ... -> codegen).
    auto fold_result = scpp::fold_immediate_calls(program);
    if (!fold_result.has_value())
        return std::unexpected(GenerateIrError{"ConstexprError", fold_result.error().what()});
    scpp::Codegen codegen("test_module");
    auto generate_result = codegen.generate(program);
    if (!generate_result.has_value())
        return std::unexpected(GenerateIrError{"CodegenError", generate_result.error().what()});
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

} // namespace

int main() {
    run_test_case_files();
    test_generate_returns_engaged_expected_on_success();
    test_generate_returns_disengaged_expected_on_failure_without_throwing();
    run_constexpr_engine_direct_api_tests();
    run_switch_end_block_reachability_tests();

    run_constexpr_error_copy_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All codegen tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}

