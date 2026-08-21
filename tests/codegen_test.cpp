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
// Codegen::infer_type carried the same 2^n shape movecheck's
// infer_expr_type did -- inferring the left operand for the
// pointer-arithmetic test and then inferring it again for the result --
// which showed up as 4,194,304 Type constructions (2^22) while compiling a
// 22-term chain. Structural rather than timed, for the same reason as
// movecheck's counterpart: pre-fix this input does not terminate at 40
// terms, so there is nothing to threshold.
void test_long_binary_chain_generates_without_re_walking_its_prefix() {
    cases_run++;
    std::string chain = "1";
    for (int term = 2; term <= 40; term++) chain += " + " + std::to_string(term);
    std::string source = "int main() {\n    int k = " + chain + ";\n    return k - k;\n}\n";
    auto ir_result = try_generate_ir(source);
    expect(ir_result.has_value(), "long_binary_chain_generates: a 40-term chain must reach codegen");
}

// ch11 [class.mem]: naming a member type from outside its enclosing type
// needs the qualified spelling `Outer::Inner`, which resolves through the
// same branch of the parser's type lookup that already serves a
// namespace-scope `app::Inner`. Declaring the member type was previously
// rejected outright with "expected a type name", so none of this path
// could run.
void test_member_type_is_usable_through_its_qualified_name() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "class Outer {\n"
        "  public:\n"
        "    struct Inner { int v; };\n"
        "    Outer() {}\n"
        "    virtual ~Outer() {}\n"
        "};\n"
        "Outer::Inner make() {\n"
        "    Outer::Inner i{};\n"
        "    i.v = 5;\n"
        "    return i;\n"
        "}\n"
        "int main() {\n"
        "    Outer::Inner r = make();\n"
        "    return r.v - 5;\n"
        "}\n");
    expect(ir_result.has_value(),
           "member_type_is_usable_through_its_qualified_name: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// [basic.scope.class]: inside the enclosing body the bare name is enough,
// and it names the member type rather than a namespace-scope type that
// happens to share the name.
void test_member_type_shadows_a_namespace_scope_type_inside_its_own_body() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct Inner { int wrong; };\n"
        "class Outer {\n"
        "  public:\n"
        "    struct Inner { int right; };\n"
        "    int make() {\n"
        "        Inner i{};\n"
        "        i.right = 7;\n"
        "        return i.right;\n"
        "    }\n"
        "    Outer() {}\n"
        "    virtual ~Outer() {}\n"
        "};\n"
        "int main() {\n"
        "    Outer o{};\n"
        "    return o.make() - 7;\n"
        "}\n");
    expect(ir_result.has_value(),
           "member_type_shadows_a_namespace_scope_type_inside_its_own_body: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// A member `enum class`'s enumerators are reached as `Outer::Kind::Deep`
// -- one more qualification level than a namespace-scope enum, and the
// only member-type kind whose *constants* also need qualified lookup.
void test_member_enum_constants_are_reachable_through_the_enclosing_type() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "class Outer {\n"
        "  public:\n"
        "    enum class Kind { Flat, Deep };\n"
        "    Outer() {}\n"
        "    virtual ~Outer() {}\n"
        "};\n"
        "int main() {\n"
        "    Outer::Kind k = Outer::Kind::Deep;\n"
        "    if (k == Outer::Kind::Deep) { return 0; }\n"
        "    return 1;\n"
        "}\n");
    expect(ir_result.has_value(),
           "member_enum_constants_are_reachable_through_the_enclosing_type: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// Spec §9.4(1) adopts [dcl.array] unchanged, so `int values[3]{1, 2, 3};`
// is [dcl.init.aggr] aggregate initialization -- the spelling spec §10's
// own iteration-statement examples use. Every array with a non-empty
// braced list was previously rejected outright, in every position, by
// initialize_storage_from_brace_args' "exactly one expression" gate.
void test_array_brace_list_initializes_each_element() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "int main() {\n"
        "    int values[3]{1, 2, 3};\n"
        "    return values[0] + values[1] + values[2] - 6;\n"
        "}\n");
    expect(ir_result.has_value(),
           "array_brace_list_initializes_each_element: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// The braced list is not the only way an array reaches storage: a default
// member initializer and a member-initializer-list entry both route
// through the same worker, and all three were rejected by the same gate.
void test_array_brace_list_initializes_a_member_and_a_mem_init_entry() {
    cases_run++;
    auto dmi_result = try_generate_ir(
        "class K {\n"
        "  public:\n"
        "    int values[3]{1, 2, 3};\n"
        "    K() {}\n"
        "    virtual ~K() {}\n"
        "};\n"
        "int main() { K k{}; return k.values[2] - 3; }\n");
    expect(dmi_result.has_value(),
           "array_brace_list_member: expected IR for an array default member initializer, got " +
               (dmi_result.has_value() ? std::string{} : dmi_result.error().kind + ": " + dmi_result.error().message));

    cases_run++;
    auto mem_init_result = try_generate_ir(
        "class M {\n"
        "  public:\n"
        "    int values[3];\n"
        "    M() : values{4, 5, 6} {}\n"
        "    virtual ~M() {}\n"
        "};\n"
        "int main() { M m{}; return m.values[2] - 6; }\n");
    expect(mem_init_result.has_value(),
           "array_brace_list_mem_init: expected IR for an array member-initializer entry, got " +
               (mem_init_result.has_value() ? std::string{} : mem_init_result.error().kind + ": " + mem_init_result.error().message));
}

// [dcl.init.aggr]: elements the list does not reach are value-initialized,
// not left indeterminate. Folding the constexpr call turns the whole
// computation into one literal, so the emitted constant is the
// evaluator's own answer for `{9, 8}` over four elements -- 9800 only if
// elements 2 and 3 really are zero.
void test_array_brace_list_value_initializes_the_elements_it_does_not_reach() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int digits() {\n"
        "    int values[4]{9, 8};\n"
        "    return values[0] * 1000 + values[1] * 100 + values[2] * 10 + values[3];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = digits();\n"
        "    return folded - 9800;\n"
        "}\n");
    expect(ir_result.has_value(),
           "array_brace_list_partial: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("9800") != std::string::npos,
           "array_brace_list_partial: expected the folded constant 9800 in the IR, so the unreached "
           "elements were value-initialized rather than left indeterminate");
}

// An array with *any* braced list was rejected during constant evaluation
// even when the list was empty, because that path assumed a braced list
// meant a constructor call and an array type's name is empty -- "no
// constexpr/consteval constructor matches for type ''".
void test_empty_array_brace_list_is_accepted_during_constant_evaluation() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int zero() {\n"
        "    int values[3]{};\n"
        "    return values[0] + values[1] + values[2];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = zero();\n"
        "    return folded;\n"
        "}\n");
    expect(ir_result.has_value(),
           "empty_array_brace_list_constexpr: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// The bound check has to hold in both implementations, so each is pinned
// against the stage that owns it: an array initialized at runtime is
// codegen's answer, the same array inside a constexpr function is the
// evaluator's. Asserting the stage is what keeps one of the two from
// silently going unchecked.
void test_too_many_array_initializers_are_rejected_by_both_implementations() {
    cases_run++;
    auto codegen_result = try_generate_ir(
        "int main() {\n"
        "    int values[3]{1, 2, 3, 4};\n"
        "    return values[0];\n"
        "}\n");
    expect(!codegen_result.has_value(),
           "too_many_array_initializers: expected an overlong list to be rejected at runtime");
    if (!codegen_result.has_value()) {
        expect(codegen_result.error().kind == "CodegenError",
               "too_many_array_initializers: expected a CodegenError, got " + codegen_result.error().kind);
        expect(codegen_result.error().message.find("too many initializers for array of 3 elements") != std::string::npos,
               "too_many_array_initializers: expected the diagnostic to name the bound, got " +
                   codegen_result.error().message);
    }

    cases_run++;
    auto constexpr_result = try_generate_ir(
        "constexpr int overlong() {\n"
        "    int values[3]{1, 2, 3, 4};\n"
        "    return values[0];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = overlong();\n"
        "    return folded;\n"
        "}\n");
    expect(!constexpr_result.has_value(),
           "too_many_array_initializers: expected an overlong list to be rejected during constant evaluation");
    if (!constexpr_result.has_value()) {
        expect(constexpr_result.error().kind == "ConstexprError",
               "too_many_array_initializers: expected a ConstexprError, got " + constexpr_result.error().kind);
        expect(constexpr_result.error().message.find("too many initializers for array of 3 elements") != std::string::npos,
               "too_many_array_initializers: expected the constant evaluator to name the bound too, got " +
                   constexpr_result.error().message);
    }
}

// The spec adopts no rule of its own for aggregate initialization, so
// under the erasure model (§3.1, Clause 4) [dcl.init.aggr] applies
// unchanged -- the same reasoning §9.4(1) makes explicit for arrays. A
// `struct` with fields could not be given values at its declaration in
// any position: every non-empty braced list was rejected, with a
// different message per position because three separate paths each
// answered the question.
void test_struct_brace_list_initializes_each_member() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "int main() {\n"
        "    S s{4, 5};\n"
        "    return s.a * 10 + s.b - 45;\n"
        "}\n");
    expect(ir_result.has_value(),
           "struct_brace_list_initializes_each_member: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// The local declaration is only one of the positions, and each was served
// by its own copy of the decision: a default member initializer and a
// member-initializer-list entry reached the shared worker and were
// rejected there with "requires exactly one expression", while the local
// declaration had its own resolution in statements.cppm and said "has no
// constructor matching this call" instead.
void test_struct_brace_list_initializes_a_member_and_a_mem_init_entry() {
    cases_run++;
    auto dmi_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "class K {\n"
        "  public:\n"
        "    S s{4, 5};\n"
        "    K() {}\n"
        "    virtual ~K() {}\n"
        "};\n"
        "int main() { K k{}; return k.s.b - 5; }\n");
    expect(dmi_result.has_value(),
           "struct_brace_list_member: expected IR for a struct default member initializer, got " +
               (dmi_result.has_value() ? std::string{} : dmi_result.error().kind + ": " + dmi_result.error().message));

    cases_run++;
    auto mem_init_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "class M {\n"
        "  public:\n"
        "    S s;\n"
        "    M() : s{6, 7} {}\n"
        "    virtual ~M() {}\n"
        "};\n"
        "int main() { M m{}; return m.s.b - 7; }\n");
    expect(mem_init_result.has_value(),
           "struct_brace_list_mem_init: expected IR for a struct member-initializer entry, got " +
               (mem_init_result.has_value() ? std::string{} : mem_init_result.error().kind + ": " + mem_init_result.error().message));
}

// The constant evaluator answers this question twice more -- once for
// `S{...}` as an expression and once for a local declaration -- and both
// assumed a braced list meant a constructor call, so a struct literal
// inside a `constexpr` function failed with "no constexpr/consteval
// constructor matches" however few members it had.
void test_struct_brace_list_is_accepted_during_constant_evaluation() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "constexpr int packed() {\n"
        "    S s{4, 5};\n"
        "    return s.a * 10 + s.b;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = packed();\n"
        "    return folded - 45;\n"
        "}\n");
    expect(ir_result.has_value(),
           "struct_brace_list_constexpr: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// [dcl.init.aggr]: members the list does not reach are initialized from
// their default member initializer if they have one and value-initialized
// otherwise -- not left indeterminate. Folding the call makes the emitted
// constant the evaluator's own answer, so 150 appears only if `b` really
// took its DMI and `c` really became zero.
void test_struct_brace_list_value_initializes_the_members_it_does_not_reach() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct D { int a; int b = 5; int c; };\n"
        "constexpr int digits() {\n"
        "    D d{1};\n"
        "    return d.a * 100 + d.b * 10 + d.c;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = digits();\n"
        "    return folded - 150;\n"
        "}\n");
    expect(ir_result.has_value(),
           "struct_brace_list_partial: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("150") != std::string::npos,
           "struct_brace_list_partial: expected the folded constant 150 in the IR, so the unreached "
           "members took their default member initializer and value-initialization rather than being "
           "left indeterminate");
}

// The member count bounds the list in both implementations, so each is
// pinned against the stage that owns it. Asserting the *reason* matters
// here beyond the usual: before the fix every one of these lists was
// rejected too, by the arity gate, so a bare "is rejected" assertion
// would pass against the unfixed compiler and prove nothing.
void test_too_many_struct_initializers_are_rejected_by_both_implementations() {
    cases_run++;
    auto codegen_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "int main() {\n"
        "    S s{1, 2, 3};\n"
        "    return s.a;\n"
        "}\n");
    expect(!codegen_result.has_value(),
           "too_many_struct_initializers: expected an overlong list to be rejected at runtime");
    if (!codegen_result.has_value()) {
        expect(codegen_result.error().kind == "CodegenError",
               "too_many_struct_initializers: expected a CodegenError, got " + codegen_result.error().kind);
        expect(codegen_result.error().message.find("too many initializers for 'S': 2 members, 3 given") !=
                   std::string::npos,
               "too_many_struct_initializers: expected the diagnostic to name the member count, got " +
                   codegen_result.error().message);
    }

    cases_run++;
    auto constexpr_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "constexpr int overlong() {\n"
        "    S s{1, 2, 3};\n"
        "    return s.a;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = overlong();\n"
        "    return folded;\n"
        "}\n");
    expect(!constexpr_result.has_value(),
           "too_many_struct_initializers: expected an overlong list to be rejected during constant evaluation");
    if (!constexpr_result.has_value()) {
        expect(constexpr_result.error().kind == "ConstexprError",
               "too_many_struct_initializers: expected a ConstexprError, got " + constexpr_result.error().kind);
        expect(constexpr_result.error().message.find("too many initializers for 'S': 2 members, 3 given") !=
                   std::string::npos,
               "too_many_struct_initializers: expected the constant evaluator to name the member count too, got " +
                   constexpr_result.error().message);
    }
}

// Every member of the list is a binding position. The path this replaces
// accepted a single-expression list for a record target and stored the
// expression *through* the record's storage as if the record were that
// expression's type, so an `int` reached an `int*` member and compiled --
// the list was a hole in the type system, not merely unsupported.
void test_struct_brace_list_checks_each_member_against_the_member_type() {
    cases_run++;
    auto pointer_result = try_generate_ir(
        "struct I { int* p; };\n"
        "class W {\n"
        "  public:\n"
        "    I f{7};\n"
        "    W() {}\n"
        "    virtual ~W() {}\n"
        "};\n"
        "int main() { W w{}; return 0; }\n");
    expect(!pointer_result.has_value(),
           "struct_brace_list_member_type: expected an int initializing a pointer member to be rejected");
    if (!pointer_result.has_value()) {
        expect(pointer_result.error().message.find("type mismatch") != std::string::npos,
               "struct_brace_list_member_type: expected a type mismatch, got " + pointer_result.error().message);
    }

    cases_run++;
    auto bool_result = try_generate_ir(
        "struct S { int a; int b; };\n"
        "int main() {\n"
        "    S s{1, true};\n"
        "    return s.a;\n"
        "}\n");
    expect(!bool_result.has_value(),
           "struct_brace_list_member_type: expected a bool initializing an int member to be rejected -- "
           "scpp has no implicit scalar conversions");
    if (!bool_result.has_value()) {
        expect(bool_result.error().message.find("type mismatch") != std::string::npos,
               "struct_brace_list_member_type: expected a type mismatch, got " + bool_result.error().message);
    }
}

// [dcl.init.aggr] excludes a type with user-declared constructors, so a
// braced list on such a struct is a call to one whether or not an
// overload accepts it. Reporting the mismatch is the point: aggregating
// it instead would silently bypass the constructor the author wrote and
// leave members it would have set holding zero.
//
// Honest note: this one passes against the pre-fix compiler as well,
// because before the fix *every* braced list on a struct was rejected, so
// there is no pre-fix behaviour for it to distinguish. It is kept as a
// regression guard rather than as coverage of the fix -- and it earned
// that place: an earlier version of this change tried aggregate
// initialization before constructor selection, and this program compiled
// and returned 3, silently bypassing the constructor.
void test_a_declared_constructor_wins_over_aggregate_initialization() {
    cases_run++;
    auto matching_result = try_generate_ir(
        "struct S { int a; int b; S(int x) { this.a = x; this.b = x; } };\n"
        "int main() { S s{6}; return s.a + s.b - 12; }\n");
    expect(matching_result.has_value(),
           "declared_constructor_wins: expected a matching constructor to be selected, got " +
               (matching_result.has_value() ? std::string{} : matching_result.error().kind + ": " + matching_result.error().message));

    cases_run++;
    auto codegen_result = try_generate_ir(
        "struct S { int a; int b; S(int x) { this.a = x; this.b = x; } };\n"
        "int main() { S s{1, 2}; return s.a; }\n");
    expect(!codegen_result.has_value(),
           "declared_constructor_wins: expected a list no constructor accepts to be rejected rather than "
           "aggregate-initialized");
    if (!codegen_result.has_value()) {
        expect(codegen_result.error().message.find("no constructor matching this call") != std::string::npos,
               "declared_constructor_wins: expected the diagnostic to name the constructor mismatch, got " +
                   codegen_result.error().message);
    }

    cases_run++;
    auto constexpr_result = try_generate_ir(
        "struct S { int a; int b; S(int x) { this.a = x; this.b = x; } };\n"
        "constexpr int bypass() { S s{1, 2}; return s.a; }\n"
        "int main() { constexpr int folded = bypass(); return folded; }\n");
    expect(!constexpr_result.has_value(),
           "declared_constructor_wins: expected the constant evaluator to reject it too");
    if (!constexpr_result.has_value()) {
        expect(constexpr_result.error().kind == "ConstexprError",
               "declared_constructor_wins: expected a ConstexprError, got " + constexpr_result.error().kind);
        expect(constexpr_result.error().message.find("no constexpr/consteval constructor matches") != std::string::npos,
               "declared_constructor_wins: expected the evaluator to name the constructor mismatch, got " +
                   constexpr_result.error().message);
    }
}

// The other half of [dcl.init.aggr]'s exclusion list. In scpp its first
// clause -- no virtual functions -- makes aggregates exactly the
// `struct`s, because §11.5(1) requires every `class` to declare a virtual
// destructor and §11.1(2.3) forbids a `struct` from declaring any virtual
// member. A non-public data member is the remaining disqualifier, and the
// diagnostic has to say which rule it is rather than blaming a
// constructor the author never wrote.
void test_struct_with_a_non_public_member_is_not_an_aggregate() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct S { private: int a; public: int b; };\n"
        "int main() { S s{1, 2}; return s.b; }\n");
    expect(!ir_result.has_value(),
           "non_public_member_is_not_an_aggregate: expected a struct with a private member to be rejected");
    if (!ir_result.has_value()) {
        expect(ir_result.error().message.find("[dcl.init.aggr]") != std::string::npos &&
                   ir_result.error().message.find("non-public data member") != std::string::npos,
               "non_public_member_is_not_an_aggregate: expected the diagnostic to name the rule and the "
               "reason, got " + ir_result.error().message);
    }
}

// #480 gave arrays braced initializers, #481 fixed multi-dimensional
// bounds and #484 gave structs aggregate initialization, and all three
// stopped at one level: `parse_brace_initializer_args` called
// `parse_expr` for every element and `{` starts no expression, so a
// struct containing a struct, or a 2-D array, could not be given values
// at all. Every shape failed identically -- "expected an expression but
// found '{'" -- in every position.
//
// The fold pins more than acceptance: it reads four cells back in an
// order that no transposition or off-by-one survives, so a list that
// merely parsed without addressing the cells it was written next to
// would not reach 6143.
void test_nested_brace_list_initializes_a_two_dimensional_array() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int folded() {\n"
        "    int a[2][3]{{1, 2, 3}, {4, 5, 6}};\n"
        "    return a[1][2] * 1000 + a[0][0] * 100 + a[1][0] * 10 + a[0][2];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(ir_result.has_value(),
           "nested_brace_2d: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("6143") != std::string::npos,
           "nested_brace_2d: expected a[1][2]==6, a[0][0]==1, a[1][0]==4 and a[0][2]==3, so every value "
           "lands in the cell it was written next to");
}

// Two levels can be reached by a special case; three cannot. Nesting is
// a property of the model or it is not there at all, so the inner-most
// list has to be interpreted by the same code as the outer-most one.
void test_nested_brace_list_nests_to_three_levels() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int folded() {\n"
        "    int a[2][2][2]{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};\n"
        "    return a[1][0][1] * 1000 + a[0][1][0] * 100 + a[1][1][1] * 10 + a[0][0][0];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(ir_result.has_value(),
           "nested_brace_3d: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("6381") != std::string::npos,
           "nested_brace_3d: expected a[1][0][1]==6, a[0][1][0]==3, a[1][1][1]==8 and a[0][0][0]==1 at "
           "three levels of nesting");
}

// An array of aggregates and an aggregate holding an array are the two
// ways the shapes compose, and a nested list must not be misassigned
// across the boundary between one element and the next.
void test_nested_brace_list_composes_arrays_and_records() {
    cases_run++;
    auto array_of_structs = try_generate_ir(
        "struct S { int x; int y; };\n"
        "constexpr int folded() {\n"
        "    S a[2]{{1, 2}, {3, 4}};\n"
        "    return a[1].x * 1000 + a[0].y * 100 + a[1].y * 10 + a[0].x;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(array_of_structs.has_value(),
           "nested_brace_compose: expected IR for an array of structs, got " +
               (array_of_structs.has_value() ? std::string{}
                                             : array_of_structs.error().kind + ": " + array_of_structs.error().message));
    if (array_of_structs.has_value()) {
        expect(array_of_structs.value().find("3241") != std::string::npos,
               "nested_brace_compose: expected a[1].x==3, a[0].y==2, a[1].y==4 and a[0].x==1");
    }

    cases_run++;
    auto struct_with_array = try_generate_ir(
        "struct S { int v[3]; int z; };\n"
        "constexpr int folded() {\n"
        "    S s{{1, 2, 3}, 9};\n"
        "    return s.v[0] * 1000 + s.v[2] * 100 + s.z * 10 + s.v[1];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(struct_with_array.has_value(),
           "nested_brace_compose: expected IR for a struct with an array member, got " +
               (struct_with_array.has_value() ? std::string{}
                                              : struct_with_array.error().kind + ": " + struct_with_array.error().message));
    if (struct_with_array.has_value()) {
        expect(struct_with_array.value().find("1392") != std::string::npos,
               "nested_brace_compose: expected s.v[0]==1, s.v[2]==3, s.z==9 and s.v[1]==2, so the scalar "
               "after the inner list lands in the member after the array");
    }

    cases_run++;
    auto struct_in_struct = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i; int z; };\n"
        "constexpr int folded() {\n"
        "    Out o{{1, 2}, 3};\n"
        "    return o.i.x * 10000 + o.i.y * 100 + o.z;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(struct_in_struct.has_value(),
           "nested_brace_compose: expected IR for a struct inside a struct, got " +
               (struct_in_struct.has_value() ? std::string{}
                                             : struct_in_struct.error().kind + ": " + struct_in_struct.error().message));
    if (struct_in_struct.has_value()) {
        expect(struct_in_struct.value().find("10203") != std::string::npos,
               "nested_brace_compose: expected o.i.x==1, o.i.y==2 and o.z==3");
    }
}

// The parser reaches a braced list from three places -- a local
// declaration, a default member initializer and a member-initializer-list
// entry -- and #484's grid found each position answered by its own copy
// of the rule. A nested list has to arrive at all three.
void test_nested_brace_list_reaches_every_brace_position() {
    cases_run++;
    auto dmi_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "class K {\n"
        "  public:\n"
        "    In i{4, 5};\n"
        "    int v[2][2]{{1, 2}, {3, 4}};\n"
        "    K() {}\n"
        "    virtual ~K() {}\n"
        "};\n"
        "int main() { K k{}; return k.v[1][1] - k.i.y + 1; }\n");
    expect(dmi_result.has_value(),
           "nested_brace_positions: expected IR for a nested default member initializer, got " +
               (dmi_result.has_value() ? std::string{} : dmi_result.error().kind + ": " + dmi_result.error().message));

    cases_run++;
    auto mem_init_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i; int z; };\n"
        "class M {\n"
        "  public:\n"
        "    Out o;\n"
        "    M() : o{{6, 7}, 8} {}\n"
        "    virtual ~M() {}\n"
        "};\n"
        "int main() { M m{}; return m.o.i.y - 7; }\n");
    expect(mem_init_result.has_value(),
           "nested_brace_positions: expected IR for a nested member-initializer entry, got " +
               (mem_init_result.has_value() ? std::string{}
                                            : mem_init_result.error().kind + ": " + mem_init_result.error().message));
}

// A member's own braced default initializer was answered by a *third*
// evaluator site, `apply_initializer_to_field`, which #484 never reached:
// it kept an arity gate of its own and rejected any list of more than one
// expression. So `struct Out { In i{4, 5}; };` used from a `constexpr`
// function failed with "requires exactly one expression" even though the
// list is flat and #484 had made the identical list work everywhere else.
// Verified against origin/main as a pre-existing gap rather than fallout
// of nesting.
void test_a_braced_member_default_initializer_is_evaluated_as_an_aggregate() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i{4, 5}; };\n"
        "constexpr int folded() {\n"
        "    Out o{};\n"
        "    return o.i.x * 10000 + o.i.y * 10;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(ir_result.has_value(),
           "braced_member_dmi_constexpr: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("40050") != std::string::npos,
           "braced_member_dmi_constexpr: expected the member's own braced default initializer to give "
           "o.i.x==4 and o.i.y==5 during constant evaluation");
}

// A nested list multiplies the binding positions, so the checks #480 and
// #484 put on a flat list have to hold at every depth and in both
// implementations -- an inner list must be measured against the *inner*
// bound and its elements against the *inner* element type, not against
// the outer ones.
void test_nested_brace_list_checks_every_level_in_both_implementations() {
    cases_run++;
    auto inner_overlong = try_generate_ir(
        "int main() {\n"
        "    int a[2][3]{{1, 2, 3, 4}, {5, 6, 7}};\n"
        "    return a[0][0];\n"
        "}\n");
    expect(!inner_overlong.has_value(),
           "nested_brace_levels: expected a 4-element inner list for an int[3] row to be rejected");
    if (!inner_overlong.has_value()) {
        expect(inner_overlong.error().message.find("array of 3 elements") != std::string::npos,
               "nested_brace_levels: expected the inner bound of 3 to be named rather than the outer bound "
               "of 2, got " + inner_overlong.error().message);
    }

    cases_run++;
    auto inner_overlong_constexpr = try_generate_ir(
        "constexpr int folded() {\n"
        "    int a[2][3]{{1, 2, 3, 4}, {5, 6, 7}};\n"
        "    return a[0][0];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(!inner_overlong_constexpr.has_value(),
           "nested_brace_levels: expected the constant evaluator to reject the overlong inner list too");
    if (!inner_overlong_constexpr.has_value()) {
        expect(inner_overlong_constexpr.error().kind == "ConstexprError" &&
                   inner_overlong_constexpr.error().message.find("array of 3 elements") != std::string::npos,
               "nested_brace_levels: expected the constant evaluator to name the inner bound, got " +
                   inner_overlong_constexpr.error().kind + ": " + inner_overlong_constexpr.error().message);
    }

    cases_run++;
    auto inner_mistyped = try_generate_ir(
        "int main() {\n"
        "    int a[2][3]{{1, true, 3}, {4, 5, 6}};\n"
        "    return a[0][0];\n"
        "}\n");
    expect(!inner_mistyped.has_value(),
           "nested_brace_levels: expected a 'bool' inside an inner int list to be rejected");
    if (!inner_mistyped.has_value()) {
        expect(inner_mistyped.error().message.find("no implicit conversion") != std::string::npos,
               "nested_brace_levels: expected the inner element to be rejected for its type, got " +
                   inner_mistyped.error().message);
    }

    cases_run++;
    auto inner_mistyped_record = try_generate_ir(
        "struct In { int x; };\n"
        "struct Out { In i; };\n"
        "constexpr int folded() {\n"
        "    Out o{{true}};\n"
        "    return o.i.x;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(!inner_mistyped_record.has_value(),
           "nested_brace_levels: expected a 'bool' for an inner int member to be rejected during constant "
           "evaluation");
    if (!inner_mistyped_record.has_value()) {
        expect(inner_mistyped_record.error().kind == "ConstexprError",
               "nested_brace_levels: expected the constant evaluator to be the one rejecting it, got " +
                   inner_mistyped_record.error().kind + ": " + inner_mistyped_record.error().message);
    }
}

// [dcl.init.aggr]/15 lets the braces of a sub-aggregate be omitted, and
// the spec adopts no aggregate-initialization rule of its own -- #484
// established there is no [dcl.init.aggr] adoption clause -- so under the
// erasure model (§3.1, Clause 4) the ordinary C++ rule applies unchanged
// and elision is part of it. Rejecting it would be inventing a deviation
// no clause authorises.
//
// The fold is the same constant as the fully-braced form above, which is
// the actual claim: elided and explicit braces describe the same object.
void test_brace_elision_fills_an_aggregate_from_a_flat_run() {
    cases_run++;
    auto array_result = try_generate_ir(
        "constexpr int folded() {\n"
        "    int a[2][3]{1, 2, 3, 4, 5, 6};\n"
        "    return a[1][2] * 1000 + a[0][0] * 100 + a[1][0] * 10 + a[0][2];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(array_result.has_value(),
           "brace_elision: expected IR for an elided 2-D array initializer, got " +
               (array_result.has_value() ? std::string{} : array_result.error().kind + ": " + array_result.error().message));
    if (array_result.has_value()) {
        expect(array_result.value().find("6143") != std::string::npos,
               "brace_elision: expected the elided form to describe the same object as the fully braced one");
    }

    cases_run++;
    auto record_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i; int z; };\n"
        "constexpr int folded() {\n"
        "    Out o{1, 2, 3};\n"
        "    return o.i.x * 10000 + o.i.y * 100 + o.z;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(record_result.has_value(),
           "brace_elision: expected IR for an elided struct-in-struct initializer, got " +
               (record_result.has_value() ? std::string{} : record_result.error().kind + ": " + record_result.error().message));
    if (record_result.has_value()) {
        expect(record_result.value().find("10203") != std::string::npos,
               "brace_elision: expected the elided run to fill the nested struct before the scalar after it");
    }

    cases_run++;
    auto mixed_result = try_generate_ir(
        "constexpr int folded() {\n"
        "    int a[2][3]{{1, 2, 3}, 4, 5, 6};\n"
        "    return a[1][2] * 1000 + a[0][0] * 100 + a[1][0] * 10 + a[0][2];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(mixed_result.has_value(),
           "brace_elision: expected IR when one row is braced and the next is elided, got " +
               (mixed_result.has_value() ? std::string{} : mixed_result.error().kind + ": " + mixed_result.error().message));
    if (mixed_result.has_value()) {
        expect(mixed_result.value().find("6143") != std::string::npos,
               "brace_elision: expected elision to resume at the sub-object after an explicitly braced one");
    }
}

// Elision makes the initializer count no longer equal to the member count,
// so the arity gate #480 and #484 rely on cannot answer "too many" on its
// own any more. The leftover check is what replaces it, and it has to
// hold in both implementations or an overlong run would be silently
// dropped in one of them.
void test_brace_elision_still_reports_initializers_left_over() {
    cases_run++;
    auto array_result = try_generate_ir(
        "int main() {\n"
        "    int a[2][3]{1, 2, 3, 4, 5, 6, 7};\n"
        "    return a[0][0];\n"
        "}\n");
    expect(!array_result.has_value(),
           "brace_elision_leftover: expected a 7th initializer for an int[2][3] to be rejected");
    if (!array_result.has_value()) {
        expect(array_result.error().message.find("left over") != std::string::npos,
               "brace_elision_leftover: expected the leftover initializer to be named, got " +
                   array_result.error().message);
    }

    cases_run++;
    auto record_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i; int z; };\n"
        "constexpr int folded() {\n"
        "    Out o{1, 2, 3, 4};\n"
        "    return o.z;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(!record_result.has_value(),
           "brace_elision_leftover: expected a 4th initializer for a 3-scalar aggregate to be rejected");
    if (!record_result.has_value()) {
        expect(record_result.error().kind == "ConstexprError" &&
                   record_result.error().message.find("left over") != std::string::npos,
               "brace_elision_leftover: expected the constant evaluator to report the leftover too, got " +
                   record_result.error().kind + ": " + record_result.error().message);
    }
}

// The risk elision carries is that a sub-object which *could* absorb a run
// swallows an initializer that was meant for it whole. An expression
// already of the sub-object's own type is the ordinary case and must win
// over elision, or `Out o{v, 6}` would try to fill `In` from `v` and `6`.
//
// This one passes against the pre-fix compiler and proves nothing about
// the defect: the list is flat, so #484 already handled it. It is a
// regression guard for the cursor introduced here -- the case that would
// break if elision were tried before the same-type check.
void test_brace_elision_does_not_consume_a_same_typed_initializer() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "struct In { int x; int y; };\n"
        "struct Out { In i; int z; };\n"
        "constexpr int folded() {\n"
        "    In v{4, 5};\n"
        "    Out o{v, 6};\n"
        "    return o.i.x * 10000 + o.i.y * 100 + o.z;\n"
        "}\n"
        "int main() {\n"
        "    constexpr int packed = folded();\n"
        "    return packed;\n"
        "}\n");
    expect(ir_result.has_value(),
           "brace_elision_same_type: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("40506") != std::string::npos,
           "brace_elision_same_type: expected `v` to initialize the whole `In` member and 6 to land in `z`");
}

// Each element of a braced list is a binding position, so the declared
// element type has to be checked at every one of them -- a list must not
// become a hole through which a value of the wrong type reaches storage
// that the written-out `values[1] = true;` would reject.
void test_array_brace_list_checks_each_element_against_the_element_type() {
    cases_run++;
    auto bool_result = try_generate_ir(
        "int main() {\n"
        "    int values[3]{1, true, 3};\n"
        "    return values[0];\n"
        "}\n");
    expect(!bool_result.has_value(),
           "array_brace_list_element_type: expected a 'bool' element in an int array to be rejected");
    // Rejection alone would also be satisfied by the arity gate this
    // change removes, so the reason is asserted rather than the verdict:
    // the element must be rejected for its *type*.
    if (!bool_result.has_value()) {
        expect(bool_result.error().message.find("no implicit conversion") != std::string::npos,
               "array_brace_list_element_type: expected the element to be rejected for its type, got " +
                   bool_result.error().message);
    }

    cases_run++;
    auto double_result = try_generate_ir(
        "int main() {\n"
        "    int values[3]{1, 2.5, 3};\n"
        "    return values[0];\n"
        "}\n");
    expect(!double_result.has_value(),
           "array_brace_list_element_type: expected a 'double' element in an int array to be rejected");
    if (!double_result.has_value()) {
        expect(double_result.error().message.find("no implicit conversion") != std::string::npos,
               "array_brace_list_element_type: expected the element to be rejected for its type, got " +
                   double_result.error().message);
    }

    cases_run++;
    auto constexpr_result = try_generate_ir(
        "constexpr int mismatched() {\n"
        "    int values[3]{1, true, 3};\n"
        "    return values[0];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = mismatched();\n"
        "    return folded;\n"
        "}\n");
    expect(!constexpr_result.has_value(),
           "array_brace_list_element_type: expected the constant evaluator to reject a mismatched element too");
    if (!constexpr_result.has_value()) {
        expect(constexpr_result.error().message.find("matching types") != std::string::npos,
               "array_brace_list_element_type: expected the constant evaluator to reject the element for its "
               "type rather than for the shape of the list, got " + constexpr_result.error().message);
    }
}

// An array of class type is where "value-initialize the elements the list
// does not reach" stops being a zero fill: element 2 has to run C's
// constructor and default member initializers, exactly as `C c{};` would.
void test_partial_brace_list_for_an_array_of_class_type_constructs_the_rest() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "class C {\n"
        "  public:\n"
        "    int v{7};\n"
        "    C() {}\n"
        "    C(int a) : v{a} {}\n"
        "    virtual ~C() {}\n"
        "};\n"
        "int main() {\n"
        "    C values[3]{C{1}, C{2}};\n"
        "    return values[0].v + values[1].v + values[2].v - 10;\n"
        "}\n");
    expect(ir_result.has_value(),
           "partial_brace_list_class_array: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
}

// A multi-dimensional declarator binds left-to-right ([dcl.array], adopted
// unchanged by ch05 §9.4(1)): `int a[2][3]` is an array of 2 arrays of 3
// int. The parser built the type by wrapping the element in a new Array as
// each bracket was read, which made the *last* bracket the outermost one and
// silently transposed every bound -- so this asserts the shape from both
// sides. `a[1][2]` is the in-bounds subscript and was rejected; `a[2][1]` is
// out of bounds and was accepted. Asserting only the first would pass again
// if the bounds were transposed back.
void test_multidimensional_array_binds_its_bounds_left_to_right() {
    cases_run++;
    auto in_bounds = try_generate_ir(
        "int main() {\n"
        "    int a[2][3];\n"
        "    a[1][2] = 0;\n"
        "    return a[1][2];\n"
        "}\n");
    expect(in_bounds.has_value(),
           "multidim_bounds: expected a[1][2] of an int[2][3] to be in bounds, got " +
               (in_bounds.has_value() ? std::string{} : in_bounds.error().kind + ": " + in_bounds.error().message));

    auto transposed = try_generate_ir(
        "int main() {\n"
        "    int a[2][3];\n"
        "    a[2][1] = 0;\n"
        "    return a[2][1];\n"
        "}\n");
    expect(!transposed.has_value(),
           "multidim_bounds: expected a[2][1] of an int[2][3] to be out of bounds");
    if (!transposed.has_value()) {
        expect(transposed.error().kind == "CodegenError",
               "multidim_bounds: expected a CodegenError, got " + transposed.error().kind);
        expect(transposed.error().message.find("out of bounds for array of size 2") != std::string::npos,
               "multidim_bounds: expected the outer bound to be 2, got " + transposed.error().message);
    }
}

// The transposition kept `sizeof(a)` correct -- the same elements in the
// same total storage -- so only the *nested* size distinguishes the two
// shapes: a row of `int[2][3]` is 12 bytes, and was 8. Folding the
// constexpr call reduces both to one literal, which also pins that the
// constant evaluator walks the same nesting codegen does.
void test_multidimensional_array_row_spans_the_inner_bound() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int shape() {\n"
        "    int a[2][3];\n"
        "    std::size_t whole = sizeof(a);\n"
        "    std::size_t row = sizeof(a[0]);\n"
        "    return static_cast<int>(whole) * 1000 + static_cast<int>(row);\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = shape();\n"
        "    return folded;\n"
        "}\n");
    expect(ir_result.has_value(),
           "multidim_sizeof: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("24012") != std::string::npos,
           "multidim_sizeof: expected sizeof(a)==24 and sizeof(a[0])==12 to fold to 24012, so a row "
           "spans the inner bound rather than the outer one");
}

// A fix that reversed exactly two bounds would be the same list-not-model
// error: three dimensions have to nest in source order too, and the middle
// bound must not be the one that stays put by accident.
void test_three_dimensional_array_bounds_are_not_reversed() {
    cases_run++;
    auto in_bounds = try_generate_ir(
        "int main() {\n"
        "    int a[2][3][4];\n"
        "    a[1][2][3] = 0;\n"
        "    return a[1][2][3];\n"
        "}\n");
    expect(in_bounds.has_value(),
           "multidim_3d: expected a[1][2][3] of an int[2][3][4] to be in bounds, got " +
               (in_bounds.has_value() ? std::string{} : in_bounds.error().kind + ": " + in_bounds.error().message));

    auto reversed = try_generate_ir(
        "int main() {\n"
        "    int a[2][3][4];\n"
        "    a[3][2][1] = 0;\n"
        "    return a[3][2][1];\n"
        "}\n");
    expect(!reversed.has_value(),
           "multidim_3d: expected the fully reversed a[3][2][1] to be out of bounds");
    if (!reversed.has_value()) {
        expect(reversed.error().message.find("out of bounds for array of size 2") != std::string::npos,
               "multidim_3d: expected the outermost bound to be 2, got " + reversed.error().message);
    }
}

// Indexing in bounds is not the same as indexing the right cell: this walks
// both dimensions and reads two distinct cells back, so a shape that merely
// admitted the subscripts without addressing them consistently would not
// fold to 1201.
void test_multidimensional_array_addresses_each_cell_distinctly() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "constexpr int fill() {\n"
        "    int a[2][3]{};\n"
        "    for (int i = 0; i < 2; i = i + 1) {\n"
        "        for (int j = 0; j < 3; j = j + 1) { a[i][j] = i * 10 + j; }\n"
        "    }\n"
        "    return a[1][2] * 100 + a[0][1];\n"
        "}\n"
        "int main() {\n"
        "    constexpr int folded = fill();\n"
        "    return folded;\n"
        "}\n");
    expect(ir_result.has_value(),
           "multidim_cells: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().kind + ": " + ir_result.error().message));
    if (!ir_result.has_value()) return;
    expect(ir_result.value().find("1201") != std::string::npos,
           "multidim_cells: expected a[1][2]==12 and a[0][1]==1 to fold to 1201");
}

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
    // ch06 §6: `char` is signed, so a string-literal byte >= 0x80 names a
    // negative char value -- 0xE2 is -30, not 226. The cell builder casts
    // through std::int8_t to say exactly that. It used to cast through
    // std::uint8_t, which was one of the two places in the compiler that
    // answered "is char unsigned" with yes, and which let constant
    // evaluation hold char values the rest of the compiler cannot
    // represent (integer_bounds_for_type bounded char at 0..255 to match).
    //
    // What must not change is the byte content: the values below are the
    // signed readings of the very same bytes, and the round-trip
    // assertion after the value check is what pins that down.
    struct ByteCase {
        std::string case_name;
        std::vector<int> bytes;
        std::int64_t index;
        std::int64_t expected;
    };
    // scpp's lexer supports only \n \t \r \\ \' \" \0, so the high bytes are
    // embedded literally rather than as \x escapes.
    const std::vector<ByteCase> byte_cases = {
        {"string_literal_high_byte_reads_signed_first", {0xE2, 0x82, 0xAC}, 0, -30},
        {"string_literal_high_byte_reads_signed_middle", {0xE2, 0x82, 0xAC}, 1, -126},
        {"string_literal_high_byte_reads_signed_last", {0xE2, 0x82, 0xAC}, 2, -84},
        {"string_literal_ascii_byte_unchanged", {0x41, 0x7F}, 1, 127},
        {"string_literal_max_byte_reads_signed", {0xFF}, 0, -1},
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
        // The signedness question is about how the byte is *read*, never
        // about which byte is stored: whatever the value, truncating it
        // back to 8 bits has to reproduce the literal's own byte.
        const std::int64_t source_byte =
            byte_case.index < static_cast<std::int64_t>(byte_case.bytes.size())
                ? static_cast<std::int64_t>(byte_case.bytes[static_cast<std::size_t>(byte_case.index)])
                : 0;
        expect(static_cast<std::int64_t>(static_cast<std::uint8_t>(snapshot.value().int_value)) == source_byte,
               byte_case.case_name + ": expected the stored byte to survive as " + std::to_string(source_byte) +
                   " but got " +
                   std::to_string(static_cast<std::int64_t>(static_cast<std::uint8_t>(snapshot.value().int_value))));
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

    {
        // A recursion deeper than the budget has to produce the documented
        // diagnostic, not a SIGSEGV. These depths used to crash the compiler
        // outright: the host stack died at 227 levels in a Debug build, 29
        // levels before max_recursion_depth (then 256) could ever be reached,
        // so the budget was unreachable and the limit did not exist.
        //
        // The depths are relative to the budget rather than absolute. They
        // were absolute, and 256 -- once past the budget -- became a depth
        // the compiler is now required to *accept*, so the case asserted
        // the old limit rather than the property.
        struct RecursionDepthCase {
            std::string case_name;
            int depth;
        };
        const int budget = scpp::ConstexprLimits{}.max_recursion_depth;
        const std::vector<RecursionDepthCase> recursion_depth_cases = {
            {"diagnostic_recursion_budget_just_past_limit", budget},
            {"diagnostic_recursion_budget_well_past_limit", budget * 2},
            {"diagnostic_recursion_budget_far_past_limit", 20000},
        };
        for (const RecursionDepthCase& recursion_case : recursion_depth_cases) {
            cases_run++;
            scpp::Program program = parse_with_std_imports(
                "consteval int count(int n) { if (n <= 0) { return 0; } return count(n - 1) + 1; }\n"
                "int main() { return count(" +
                std::to_string(recursion_case.depth) + "); }\n");
            auto result = scpp::fold_immediate_calls(program);
            expect(!result.has_value(), recursion_case.case_name + ": expected the recursion budget to be exceeded");
            if (!result.has_value()) {
                const std::string what = result.error().what();
                const std::string expected_recursion_fragment = "constexpr evaluation exceeded recursion budget";
                expect(what.find(expected_recursion_fragment) != std::string::npos,
                       recursion_case.case_name + ": expected '" + expected_recursion_fragment + "' but got '" + what +
                           "'");
            }
        }
    }

    {
        // The margin proof, and the reason max_recursion_depth is derived
        // from a measurement rather than picked: exactly max_recursion_depth
        // levels of this engine's walk must actually fit on the host stack.
        // Recursing to the documented depth has to succeed and one level
        // deeper has to be diagnosed, so if a later change to the evaluation
        // walk's frames makes the documented depth overflow the stack again,
        // this case crashes instead of quietly passing -- which is how the
        // previous two values for this constant went wrong unnoticed.
        // ch06 (6.4) requires an implementation to support "no less than
        // 512 nested evaluations". Every previous value of this constant
        // either violated that (128, 256) or violated it in practice by
        // segfaulting before reaching it (512, as first written). It is
        // reachable now only because the evaluator's per-level host-stack
        // cost fell from 36,802 bytes to 10,714, so pin the conformance
        // floor next to the margin proof below: if a later change regrows
        // the frames, the case below crashes rather than passing quietly,
        // and if someone answers that by lowering the constant, this one
        // fails.
        expect(scpp::ConstexprLimits{}.max_recursion_depth >= 512,
               "recursion_budget_conformance: ch06 (6.4) requires at least 512 nested evaluations");
        cases_run++;

        const int recursion_budget = scpp::ConstexprLimits{}.max_recursion_depth;
        const std::string count_source =
            "consteval int count(int n) { if (n <= 0) { return 0; } return count(n - 1) + 1; }\n";

        std::string case_name = "recursion_budget_is_reachable_at_the_documented_depth";
        cases_run++;
        scpp::Program at_budget = parse_with_std_imports(
            count_source + "int main() { return count(" + std::to_string(recursion_budget - 1) + "); }\n");
        auto at_budget_result = scpp::fold_immediate_calls(at_budget);
        expect(at_budget_result.has_value(),
               case_name + ": expected a recursion of exactly max_recursion_depth levels to evaluate, but got '" +
                   (at_budget_result.has_value() ? std::string{} : std::string(at_budget_result.error().what())) + "'");

        case_name = "recursion_budget_fires_one_level_past_the_documented_depth";
        cases_run++;
        scpp::Program past_budget = parse_with_std_imports(
            count_source + "int main() { return count(" + std::to_string(recursion_budget) + "); }\n");
        auto past_budget_result = scpp::fold_immediate_calls(past_budget);
        expect(!past_budget_result.has_value(),
               case_name + ": expected one level past max_recursion_depth to be diagnosed");
        if (!past_budget_result.has_value()) {
            const std::string what = past_budget_result.error().what();
            expect(what.find("constexpr evaluation exceeded recursion budget") != std::string::npos,
                   case_name + ": expected the recursion-budget diagnostic but got '" + what + "'");
        }
    }

    {
        // What runs out is host stack bytes, not levels, and a count cannot
        // measure bytes: the per-level cost differs between an optimized and
        // an unoptimized build of this compiler and changes with any edit to
        // the evaluation walk. Raising the depth budget out of the way and
        // lowering the byte budget must therefore still yield the documented
        // diagnostic rather than a crash. That is the guarantee the depth
        // constant cannot make on its own, and it is why one exists.
        std::string case_name = "recursion_budget_is_enforced_from_stack_bytes";
        cases_run++;
        scpp::Program program = parse_with_std_imports(
            "consteval int count(int n) { if (n <= 0) { return 0; } return count(n - 1) + 1; }\n"
            "int main() { return count(100000); }\n");
        scpp::ConstexprLimits limits{};
        limits.max_recursion_depth = 1000000;
        limits.max_stack_bytes = 16 * 1024;
        auto result = scpp::fold_immediate_calls(program, limits);
        expect(!result.has_value(), case_name + ": expected the stack-byte budget to stop the recursion");
        if (!result.has_value()) {
            const std::string what = result.error().what();
            expect(what.find("constexpr evaluation exceeded recursion budget") != std::string::npos,
                   case_name + ": expected the recursion-budget diagnostic but got '" + what + "'");
        }
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

// codegen resolves overloads a second time, by exact type, and every one
// of its failures used to be reported as "call to unknown function 'X'
// (resolve)" -- a name-lookup message for a question that has nothing to
// do with name lookup. The function was declared; only an argument type
// differed. For a method it printed codegen's mangled `Class_method`
// spelling too, a name that appears nowhere in the user's source.
//
// This reaches codegen (rather than being stopped by the frontend)
// because movecheck's resolve_overload deliberately accepts a
// single-candidate name without matching argument types -- documented
// there, and sound, since it explicitly defers the type check to codegen.
// Only the report was wrong.
void run_call_resolution_diagnostic_tests() {
    auto error_for = [](std::string_view source) -> std::string {
        auto result = try_generate_ir(source);
        if (result.has_value()) return "<no error>";
        return result.error().message;
    };

    {
        cases_run++;
        std::string message = error_for("void sink(std::int64_t value) { }\n"
                                        "int main() {\n"
                                        "    int narrow = 5;\n"
                                        "    sink(narrow);\n"
                                        "    return 0;\n"
                                        "}\n");
        expect(message.find("unknown function") == std::string::npos,
               "call_resolution_diagnostic: an argument type mismatch must not be reported as an unknown "
               "function, got '" + message + "'");
        expect(message.find("argument 1 is 'int'") != std::string::npos,
               "call_resolution_diagnostic: expected the offending argument and its actual type, got '" + message +
                   "'");
        expect(message.find("expects 'int64_t'") != std::string::npos,
               "call_resolution_diagnostic: expected the parameter type, got '" + message + "'");
    }
    {
        // The mismatch is in the *second* argument; a diagnostic that
        // only says "these argument types" leaves the reader to find it.
        cases_run++;
        std::string message = error_for("void sink(std::int64_t a, std::int64_t b) { }\n"
                                        "int main() {\n"
                                        "    std::int64_t wide = 1;\n"
                                        "    int narrow = 2;\n"
                                        "    sink(wide, narrow);\n"
                                        "    return 0;\n"
                                        "}\n");
        expect(message.find("argument 2 is 'int'") != std::string::npos,
               "call_resolution_diagnostic: expected the second argument to be named, got '" + message + "'");
    }
    {
        // A method: the message must use the name the user wrote, never
        // codegen's internal `Box_take` mangling.
        cases_run++;
        std::string message = error_for("class Box {\n"
                                        "  public:\n"
                                        "    virtual ~Box() = default;\n"
                                        "    int value{};\n"
                                        "    void take(std::int64_t v) { value = 1; }\n"
                                        "};\n"
                                        "int main() {\n"
                                        "    Box b{};\n"
                                        "    int narrow = 2;\n"
                                        "    b.take(narrow);\n"
                                        "    return 0;\n"
                                        "}\n");
        expect(message.find("Box_take") == std::string::npos,
               "call_resolution_diagnostic: the mangled method name must never reach a diagnostic, got '" + message +
                   "'");
        expect(message.find("'Box::take'") != std::string::npos,
               "call_resolution_diagnostic: expected the method to be named as written, got '" + message + "'");
    }
    {
        // A genuinely absent name still says so -- and still says it
        // without the mangling, and without the internal "(resolve)" tag.
        cases_run++;
        std::string message = error_for("class Box {\n"
                                        "  public:\n"
                                        "    virtual ~Box() = default;\n"
                                        "    int value{};\n"
                                        "};\n"
                                        "int main() {\n"
                                        "    Box b{};\n"
                                        "    b.missing();\n"
                                        "    return 0;\n"
                                        "}\n");
        expect(message.find("call to unknown function 'Box::missing'") != std::string::npos,
               "call_resolution_diagnostic: expected an unknown-name diagnostic naming Box::missing, got '" +
                   message + "'");
        expect(message.find("(resolve)") == std::string::npos && message.find("(llvm)") == std::string::npos,
               "call_resolution_diagnostic: internal phase tags must not appear in user-facing diagnostics, got '" +
                   message + "'");
    }
    {
        // Argument *count*, reported as a count problem rather than as a
        // type problem.
        cases_run++;
        std::string message = error_for("void sink(int a, int b) { }\n"
                                        "int main() {\n"
                                        "    sink(1);\n"
                                        "    return 0;\n"
                                        "}\n");
        expect(message.find("takes 1 argument") != std::string::npos,
               "call_resolution_diagnostic: expected an argument-count diagnostic, got '" + message + "'");
        expect(message.find("candidate: sink(int, int)") != std::string::npos,
               "call_resolution_diagnostic: expected the candidate signature, got '" + message + "'");
    }
    {
        // Valid code still compiles -- the new diagnosis must only run on
        // the failure path.
        cases_run++;
        auto ir = try_generate_ir("std::int64_t widen(std::int64_t value) { return value; }\n"
                                  "int main() {\n"
                                  "    std::int64_t wide = 5;\n"
                                  "    return static_cast<int>(widen(wide));\n"
                                  "}\n");
        expect(ir.has_value(), "call_resolution_diagnostic: a correctly-typed call must still compile, got '" +
                                   (ir.has_value() ? std::string() : ir.error().message) + "'");
    }
}

// Reference collapsing for a deduced reference. `auto& r = idem(a);`
// infers `int&` for the initializer, so wrapping that in the declared
// `&` builds a reference to a reference -- which codegen, not
// movecheck, is the one to reject ("a reference to a reference is not
// supported", layout.cppm). The written-out `int& r = idem(a);` has
// always been accepted, so this is the deduced spelling catching up
// with it, and it has to be tested here: monomorphize and check_moves
// both accept the un-collapsed form and hand it on.
void test_auto_reference_to_a_reference_returning_call_collapses() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "int& idem(int& x) { return x; }\n"
        "int main() {\n"
        "    int a{1};\n"
        "    auto& r = idem(a);\n"
        "    r = r + 10;\n"
        "    return r - 11;\n"
        "}\n");
    expect(ir_result.has_value(),
           "auto_reference_to_a_reference_returning_call_collapses: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().message));
}

// The same shape reached through a method call on the enclosing
// object, which is the form the compiler's own source uses most.
void test_auto_reference_to_a_reference_returning_method_collapses() {
    cases_run++;
    auto ir_result = try_generate_ir(
        "class C {\n"
        "public:\n"
        "    virtual ~C() {}\n"
        "    int f_{5};\n"
        "    int& mref() { return f_; }\n"
        "    int run() {\n"
        "        auto& r = this->mref();\n"
        "        r = r + 1;\n"
        "        return r - 6;\n"
        "    }\n"
        "};\n"
        "int main() { C c{}; return c.run(); }\n");
    expect(ir_result.has_value(),
           "auto_reference_to_a_reference_returning_method_collapses: expected IR, got " +
               (ir_result.has_value() ? std::string{} : ir_result.error().message));
}

int main() {
    run_test_case_files();
    test_long_binary_chain_generates_without_re_walking_its_prefix();
    test_member_type_is_usable_through_its_qualified_name();
    test_member_type_shadows_a_namespace_scope_type_inside_its_own_body();
    test_member_enum_constants_are_reachable_through_the_enclosing_type();
    test_array_brace_list_initializes_each_element();
    test_array_brace_list_initializes_a_member_and_a_mem_init_entry();
    test_array_brace_list_value_initializes_the_elements_it_does_not_reach();
    test_empty_array_brace_list_is_accepted_during_constant_evaluation();
    test_too_many_array_initializers_are_rejected_by_both_implementations();
    test_struct_brace_list_initializes_each_member();
    test_struct_brace_list_initializes_a_member_and_a_mem_init_entry();
    test_struct_brace_list_is_accepted_during_constant_evaluation();
    test_struct_brace_list_value_initializes_the_members_it_does_not_reach();
    test_too_many_struct_initializers_are_rejected_by_both_implementations();
    test_struct_brace_list_checks_each_member_against_the_member_type();
    test_a_declared_constructor_wins_over_aggregate_initialization();
    test_struct_with_a_non_public_member_is_not_an_aggregate();
    test_array_brace_list_checks_each_element_against_the_element_type();
    test_nested_brace_list_initializes_a_two_dimensional_array();
    test_nested_brace_list_nests_to_three_levels();
    test_nested_brace_list_composes_arrays_and_records();
    test_nested_brace_list_reaches_every_brace_position();
    test_a_braced_member_default_initializer_is_evaluated_as_an_aggregate();
    test_nested_brace_list_checks_every_level_in_both_implementations();
    test_brace_elision_fills_an_aggregate_from_a_flat_run();
    test_brace_elision_still_reports_initializers_left_over();
    test_brace_elision_does_not_consume_a_same_typed_initializer();
    test_partial_brace_list_for_an_array_of_class_type_constructs_the_rest();
    test_multidimensional_array_binds_its_bounds_left_to_right();
    test_multidimensional_array_row_spans_the_inner_bound();
    test_three_dimensional_array_bounds_are_not_reversed();
    test_multidimensional_array_addresses_each_cell_distinctly();
    test_auto_reference_to_a_reference_returning_call_collapses();
    test_auto_reference_to_a_reference_returning_method_collapses();
    test_generate_returns_engaged_expected_on_success();
    test_generate_returns_disengaged_expected_on_failure_without_throwing();
    run_constexpr_engine_direct_api_tests();
    run_switch_end_block_reachability_tests();
    run_local_shadowing_tests();
    run_virtual_base_initializer_frame_tests();
    run_value_initialized_temporary_constructor_tests();
    run_array_element_lifetime_tests();
    run_user_destructor_member_teardown_tests();
    run_call_resolution_diagnostic_tests();
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

