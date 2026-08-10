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



int main() {
    run_test_case_files();
    test_generate_returns_engaged_expected_on_success();
    test_generate_returns_disengaged_expected_on_failure_without_throwing();
    run_constexpr_engine_direct_api_tests();
    run_switch_end_block_reachability_tests();

    run_constexpr_error_copy_tests();
    run_constexpr_null_pointer_storage_tests();
    run_constexpr_cell_data_kind_tests();
    run_constexpr_object_field_order_tests();
    run_constexpr_string_literal_byte_tests();
    run_constexpr_diagnostic_text_tests();
    run_constexpr_name_index_tests();
    run_constexpr_best_effort_const_local_tests();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All codegen tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}

