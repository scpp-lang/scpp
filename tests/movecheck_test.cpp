import scpp.compiler.movecheck;
import scpp.parser;
import scpp.ast;
import std;

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

std::optional<std::string> move_error_message(std::string_view source) {
    auto parse_result = try_parse_with_std_imports(source);
    if (!parse_result.has_value()) return parse_result.error().what();
    scpp::Program program = std::move(parse_result.value());
    // ch05 §5.11: monomorphize_generics must run before check_moves,
    // exactly like the real pipeline (driver.cppm's
    // emit_object_file_for_program) -- a generic function's call
    // site is only ever type-correct against a witness-typed
    // signature *before* this rewrite; concept-satisfaction
    // rejection also only happens here, so a movetest_source case
    // exercising either would otherwise never see it.
    auto monomorphize_result = scpp::monomorphize_generics(program);
    if (!monomorphize_result.has_value()) return monomorphize_result.error().what();
    auto check_moves_result = scpp::check_moves(program);
    if (!check_moves_result.has_value()) return check_moves_result.error().what();
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

// Overload resolution used to answer every one of these with the same
// sentence -- "no overload of 'f' matches these argument types" --
// including the two cases where that sentence is factually wrong: an
// argument-*count* mismatch is not a type problem, and telling the reader
// to look at their types sends them to the wrong place entirely.
// The borrow checker used to make the *safer* spelling of a loop the
// illegal one. `for (Item& item : items)` could pass `item` to a
// `const Item&` parameter; `for (const Item& item : items)` could not,
// because only a *mutable* lender was recognized as reborrowing rather
// than borrowing afresh. Every assertion below is on the accepting side:
// the point of the fix is that sound code stops being rejected.
void test_shared_reference_can_be_lent_on_to_a_const_parameter() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "int score(const Item& item) {\n"
        "    return item.weight;\n"
        "}\n"
        "int total() {\n"
        "    std::vector<Item> items{};\n"
        "    int sum = 0;\n"
        "    for (const Item& item : items) {\n"
        "        sum += score(item);\n"
        "    }\n"
        "    return sum;\n"
        "}\n");
    expect(!error.has_value(),
           "shared_reference_can_be_lent_on_to_a_const_parameter: expected acceptance, got '" +
              error.value_or(std::string()) + "'");
}

// The same reborrow, one step removed: a shared alias bound from a
// mutable loop variable, then lent on. `alias` is derived from an access
// that has already been accounted for, so it cannot conflict with it.
void test_shared_alias_of_a_mutable_reference_can_be_lent_on() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "int score(const Item& item) {\n"
        "    return item.weight;\n"
        "}\n"
        "int total() {\n"
        "    std::vector<Item> items{};\n"
        "    int sum = 0;\n"
        "    for (Item& item : items) {\n"
        "        const Item& alias = item;\n"
        "        sum += score(alias);\n"
        "    }\n"
        "    return sum;\n"
        "}\n");
    expect(!error.has_value(), "shared_alias_of_a_mutable_reference_can_be_lent_on: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

// A shared lender still cannot satisfy a `T&` parameter: the reborrow
// path must not become a way to manufacture a mutable alias out of a
// read-only one.
void test_shared_reference_still_cannot_satisfy_a_mutable_parameter() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "void bump(Item& item) {\n"
        "    item.weight = item.weight + 1;\n"
        "    return;\n"
        "}\n"
        "void run() {\n"
        "    std::vector<Item> items{};\n"
        "    for (const Item& item : items) {\n"
        "        bump(item);\n"
        "    }\n"
        "    return;\n"
        "}\n");
    expect(error.has_value(), "shared_reference_still_cannot_satisfy_a_mutable_parameter: expected rejection");
}

// A read-only loop must not take a *mutable* borrow of the container it
// walks: the synthesized range storage only ever needs the access the
// loop variable needs.
void test_read_only_range_for_still_allows_reading_the_container() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "int total() {\n"
        "    std::vector<Item> items{};\n"
        "    int sum = 0;\n"
        "    for (const Item& item : items) {\n"
        "        sum += item.weight;\n"
        "        sum += static_cast<int>(items.size());\n"
        "    }\n"
        "    return sum;\n"
        "}\n");
    expect(!error.has_value(), "read_only_range_for_still_allows_reading_the_container: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

// A by-value loop variable needs no more than read access either -- and
// here the range expression is *itself* a reference (`at()` on a member
// in a non-const method), the shape that adopted that reference's
// mutability verbatim and so blocked any further borrow from `this`.
void test_by_value_range_for_over_a_member_accessor_allows_other_member_borrows() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "class Probe {\n"
        "  public:\n"
        "    virtual ~Probe() = default;\n"
        "    std::unordered_map<std::string, std::vector<std::size_t>> index_{};\n"
        "    std::vector<Item> items_{};\n"
        "    int find(const std::string& name) {\n"
        "        if (!index_.contains(name)) return 0;\n"
        "        int sum = 0;\n"
        "        for (std::size_t i : index_.at(name)) {\n"
        "            const Item& item = items_[i];\n"
        "            sum += item.weight;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "};\n");
    expect(!error.has_value(),
           "by_value_range_for_over_a_member_accessor_allows_other_member_borrows: expected acceptance, got '" +
              error.value_or(std::string()) + "'");
}

// The cap is on what the loop *needs*, not a blanket demotion: a
// mutable loop variable still takes a mutable borrow, so it still both
// permits mutation through the loop variable and excludes any other use
// of the container while the loop runs.
void test_mutable_range_for_still_borrows_the_container_exclusively() {
    cases_run++;
    std::optional<std::string> accepted = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "void run() {\n"
        "    std::vector<Item> items{};\n"
        "    for (Item& item : items) {\n"
        "        item.weight = 3;\n"
        "    }\n"
        "    return;\n"
        "}\n");
    expect(!accepted.has_value(), "mutable_range_for_still_borrows_the_container_exclusively: expected mutation "
                                  "through a mutable loop variable to be accepted, got '" +
                                      accepted.value_or(std::string()) + "'");

    std::optional<std::string> rejected = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "int total() {\n"
        "    std::vector<Item> items{};\n"
        "    int sum = 0;\n"
        "    for (Item& item : items) {\n"
        "        sum += static_cast<int>(items.size());\n"
        "    }\n"
        "    return sum;\n"
        "}\n");
    expect(rejected.has_value(), "mutable_range_for_still_borrows_the_container_exclusively: expected reading the "
                                 "container inside a mutable loop to stay rejected");
}

// A `const` loop variable must not become a mutation channel just
// because the range storage behind it was demoted to a shared binding.
void test_read_only_range_for_still_rejects_mutation_through_the_loop_variable() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "void run() {\n"
        "    std::vector<Item> items{};\n"
        "    for (const Item& item : items) {\n"
        "        item.weight = 3;\n"
        "    }\n"
        "    return;\n"
        "}\n");
    expect(error.has_value(),
           "read_only_range_for_still_rejects_mutation_through_the_loop_variable: expected rejection");
}

// The range storage's constness is recorded by the parser and consumed
// by monomorphize, so it has to survive the body clone that a template
// instantiation goes through -- the exact class of omission that dropped
// `const`/`static` from every instantiation before #434.
void test_read_only_range_for_survives_generic_instantiation() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Item {\n"
        "  public:\n"
        "    virtual ~Item() = default;\n"
        "    int weight{};\n"
        "};\n"
        "int score(const Item& item) {\n"
        "    return item.weight;\n"
        "}\n"
        "template <typename T>\n"
        "int total(const std::vector<T>& values) {\n"
        "    int sum = 0;\n"
        "    for (const T& value : values) {\n"
        "        sum += score(value);\n"
        "        sum += static_cast<int>(values.size());\n"
        "    }\n"
        "    return sum;\n"
        "}\n"
        "int run() {\n"
        "    std::vector<Item> items{};\n"
        "    return total<Item>(items);\n"
        "}\n");
    expect(!error.has_value(), "read_only_range_for_survives_generic_instantiation: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

// A single-argument converting constructor has to be honoured at every
// value-to-declared-class-type boundary, not just some of them. These
// four cases are the same conversion (a string literal to std::string,
// via std::string's own converting constructor) written in the four
// syntactic positions that exist; before the fix the first two were
// accepted and the last two rejected.
void test_converting_constructor_is_accepted_as_a_call_argument() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "void sink(std::string s) {\n"
        "    return;\n"
        "}\n"
        "void caller() {\n"
        "    sink(\"hi\");\n"
        "    return;\n"
        "}\n");
    expect(!error.has_value(), "converting_constructor_is_accepted_as_a_call_argument: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

void test_converting_constructor_is_accepted_as_a_return_operand() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "std::string make() {\n"
        "    return \"hi\";\n"
        "}\n");
    expect(!error.has_value(), "converting_constructor_is_accepted_as_a_return_operand: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

void test_converting_constructor_is_accepted_as_a_variable_initializer() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "void caller() {\n"
        "    std::string s = \"hi\";\n"
        "    return;\n"
        "}\n");
    expect(!error.has_value(), "converting_constructor_is_accepted_as_a_variable_initializer: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

void test_converting_constructor_is_accepted_as_a_constructor_argument() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Holder {\n"
        "public:\n"
        "    std::string s;\n"
        "    virtual ~Holder() = default;\n"
        "    Holder(std::string v) : s{std::move(v)} {}\n"
        "};\n"
        "void caller() {\n"
        "    Holder h{\"hi\"};\n"
        "    return;\n"
        "}\n");
    expect(!error.has_value(), "converting_constructor_is_accepted_as_a_constructor_argument: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

// The converting constructor must still be *selected*, not merely
// tolerated: with a second overload present the checker has to pick the
// one whose parameter the literal actually converts to.
void test_converting_constructor_is_selected_among_constructor_overloads() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Holder {\n"
        "public:\n"
        "    std::string s;\n"
        "    int n;\n"
        "    virtual ~Holder() = default;\n"
        "    Holder(std::string v) : s{std::move(v)}, n{0} {}\n"
        "    Holder(int v) : s{\"\"}, n{v} {}\n"
        "};\n"
        "void caller() {\n"
        "    Holder h{\"hi\"};\n"
        "    Holder i{7};\n"
        "    return;\n"
        "}\n");
    expect(!error.has_value(), "converting_constructor_is_selected_among_constructor_overloads: expected acceptance, got '" +
                                   error.value_or(std::string()) + "'");
}

// Widening the variable-initializer boundary must not weaken the
// same-type copy rule it sits next to: a class with a user-declared
// destructor has no copy constructor (spec §6.5(2)), and `Foo b = a;`
// stays ill-formed. `find_single_argument_converting_constructor_signature`
// skips same-type constructors precisely so this case cannot leak
// through as a "conversion".
void test_same_type_initializer_still_requires_copy_constructibility() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Foo {\n"
        "public:\n"
        "    int v;\n"
        "    virtual ~Foo() = default;\n"
        "    Foo() : v{0} {}\n"
        "};\n"
        "void caller() {\n"
        "    Foo a{};\n"
        "    Foo b = a;\n"
        "    return;\n"
        "}\n");
    expect(error.has_value() && error->find("not copy-constructible") != std::string::npos,
           "same_type_initializer_still_requires_copy_constructibility: expected a copy-constructibility "
           "diagnostic, got '" + error.value_or(std::string("<no error>")) + "'");
}

// `test_two_step_conversion_is_rejected_at_every_boundary` used to sit
// here. It asserted that a conversion needing two user-defined
// constructors chained in one implicit conversion sequence is rejected
// at every boundary -- a real rule, but not one movecheck adjudicates:
// movecheck accepts every such shape and defers to codegen. It passed
// only because its example was `return nullptr;` into
// `std::expected<std::shared_ptr<Cell>, Err>`, and `nullptr` was an
// untyped Identifier expression that matched nothing anywhere. It
// never exercised conversion chaining at all.
//
// Now that `nullptr` has type `nullptr_t`, that example is
// legitimately accepted -- see the test below. The two-step rule
// itself did not go untested: it moved to
// tests/codegentest_source/two_step_conversion_is_rejected_at_the_*_boundary,
// which uses an example that really is two-step and a harness that
// runs the phase enforcing it.

// The shape the case above used to claim was two-step. It is not, and
// it must be accepted at every boundary, matching real C++: an
// `expected` whose value type is a smart pointer is constructible from
// `nullptr`, yielding a *value*-state `expected` holding an empty
// pointer -- not an error-state one. Getting the state wrong here
// would be silent, so the acceptance alone is not enough to assert;
// the corresponding runtime behavior is pinned by the codegen case
// files.
void test_expected_of_a_smart_pointer_accepts_nullptr() {
    cases_run++;
    const std::string preamble =
        "import std;\n"
        "class Cell {\n"
        "public:\n"
        "    int v;\n"
        "    virtual ~Cell() = default;\n"
        "    Cell() : v{0} {}\n"
        "};\n"
        "class Err {\n"
        "public:\n"
        "    int code;\n"
        "    virtual ~Err() = default;\n"
        "    Err() : code{0} {}\n"
        "    Err(const Err& o) : code{o.code} {}\n"
        "};\n";
    std::optional<std::string> returned = move_error_message(
        preamble +
        "std::expected<std::shared_ptr<Cell>, Err> make() {\n"
        "    return nullptr;\n"
        "}\n");
    expect(!returned.has_value(),
           "expected_of_a_smart_pointer_accepts_nullptr: expected the return boundary to accept it" +
               (returned.has_value() ? std::string(", got '") + *returned + "'" : ""));

    std::optional<std::string> initialized = move_error_message(
        preamble +
        "void caller() {\n"
        "    std::expected<std::shared_ptr<Cell>, Err> e = nullptr;\n"
        "    return;\n"
        "}\n");
    expect(!initialized.has_value(),
           "expected_of_a_smart_pointer_accepts_nullptr: expected the initializer boundary to accept it" +
               (initialized.has_value() ? std::string(", got '") + *initialized + "'" : ""));

    std::optional<std::string> passed = move_error_message(
        preamble +
        "void sink(std::expected<std::shared_ptr<Cell>, Err> e) {\n"
        "    return;\n"
        "}\n"
        "void caller() {\n"
        "    sink(nullptr);\n"
        "    return;\n"
        "}\n");
    expect(!passed.has_value(),
           "expected_of_a_smart_pointer_accepts_nullptr: expected the argument boundary to accept it" +
               (passed.has_value() ? std::string(", got '") + *passed + "'" : ""));
}

// The residual "this initializer is not allowed" diagnostic used to
// advise `T v(args);` -- a spelling the parser categorically rejects
// ("parenthesized direct-initialization is not allowed for object
// declarations"), so following the advice produced a second error. It
// must name brace-init, which is the syntax the language actually has.
void test_rejected_initializer_diagnostic_advises_a_syntax_that_parses() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "class Foo {\n"
        "public:\n"
        "    int v;\n"
        "    virtual ~Foo() = default;\n"
        "    Foo() : v{0} {}\n"
        "};\n"
        "int side() {\n"
        "    return 1;\n"
        "}\n"
        "void caller() {\n"
        "    Foo b = side();\n"
        "    return;\n"
        "}\n");
    expect(error.has_value() && error->find("brace-init") != std::string::npos,
           "rejected_initializer_diagnostic_advises_a_syntax_that_parses: expected brace-init advice, got '" +
              error.value_or(std::string("<no error>")) + "'");
    expect(error.has_value() && error->find("(args);") == std::string::npos,
           "rejected_initializer_diagnostic_advises_a_syntax_that_parses: the diagnostic still advises "
           "parenthesized direct-initialization, which the parser rejects: '" +
              error.value_or(std::string("<no error>")) + "'");
}

// The same restriction -- `std::move(E)` records move state per named
// object (spec §6.2(3)), so a member or element has nowhere to record it
// -- used to surface as three different messages depending on which
// boundary noticed first, and two of them never mentioned std::move at
// all. The call-argument one was the worst: it advised "a fresh value
// such as std::move(x)", which is exactly what the reader wrote.
void test_move_of_a_member_reports_the_same_reason_at_every_boundary() {
    cases_run++;
    const std::string preamble =
        "import std;\n"
        "class Box {\n"
        "public:\n"
        "    std::vector<int> data{};\n"
        "    virtual ~Box() = default;\n"
        "    Box() {}\n"
        "};\n"
        "void sink(std::vector<int> v) {\n"
        "    return;\n"
        "}\n";
    const std::string expected = "std::move currently only supports a plain local variable";

    std::optional<std::string> as_argument = move_error_message(
        preamble +
        "void caller() {\n"
        "    Box b{};\n"
        "    sink(std::move(b.data));\n"
        "    return;\n"
        "}\n");
    expect(as_argument.has_value() && as_argument->find(expected) != std::string::npos,
           "move_of_a_member_reports_the_same_reason_at_every_boundary: argument boundary gave '" +
              as_argument.value_or(std::string("<no error>")) + "'");

    std::optional<std::string> as_initializer = move_error_message(
        preamble +
        "void caller() {\n"
        "    Box b{};\n"
        "    std::vector<int> local = std::move(b.data);\n"
        "    return;\n"
        "}\n");
    expect(as_initializer.has_value() && as_initializer->find(expected) != std::string::npos,
           "move_of_a_member_reports_the_same_reason_at_every_boundary: initializer boundary gave '" +
              as_initializer.value_or(std::string("<no error>")) + "'");

    std::optional<std::string> as_return = move_error_message(
        preamble +
        "std::vector<int> caller() {\n"
        "    Box b{};\n"
        "    return std::move(b.data);\n"
        "}\n");
    expect(as_return.has_value() && as_return->find(expected) != std::string::npos,
           "move_of_a_member_reports_the_same_reason_at_every_boundary: return boundary gave '" +
              as_return.value_or(std::string("<no error>")) + "'");
}

void test_overload_failure_distinguishes_arity_from_argument_type() {
    cases_run++;
    std::optional<std::string> too_few = move_error_message(
        "import std;\n"
        "void sink(int a, int b) {\n"
        "    return;\n"
        "}\n"
        "void caller() {\n"
        "    sink(1);\n"
        "    return;\n"
        "}\n");
    expect(too_few.has_value() && too_few->find("takes 1 argument") != std::string::npos,
           "overload_failure_distinguishes_arity_from_argument_type: expected an argument-count diagnostic, got '" +
              (too_few.has_value() ? *too_few : std::string("<no error>")) + "'");
    expect(too_few.has_value() && too_few->find("candidate: sink(int, int)") != std::string::npos,
           "overload_failure_distinguishes_arity_from_argument_type: expected the candidate signature to be listed, "
           "got '" + (too_few.has_value() ? *too_few : std::string("<no error>")) + "'");

    std::optional<std::string> too_many = move_error_message(
        "import std;\n"
        "void sink(int a) {\n"
        "    return;\n"
        "}\n"
        "void caller() {\n"
        "    sink(1, 2);\n"
        "    return;\n"
        "}\n");
    expect(too_many.has_value() && too_many->find("takes 2 arguments") != std::string::npos,
           "overload_failure_distinguishes_arity_from_argument_type: expected a plural argument-count diagnostic, "
           "got '" + (too_many.has_value() ? *too_many : std::string("<no error>")) + "'");
}

// With two or more overloads the frontend does check argument types, but
// it never said *which* argument was wrong, what it actually was, or what
// was expected -- everything the reader needs in order to act on it.
void test_overload_failure_names_the_offending_argument_and_types() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "void sink(std::int64_t a, std::int64_t b) {\n"
        "    return;\n"
        "}\n"
        "void sink(bool a, bool b) {\n"
        "    return;\n"
        "}\n"
        "void caller() {\n"
        "    std::int64_t wide = 1;\n"
        "    int narrow = 2;\n"
        "    sink(wide, narrow);\n"
        "    return;\n"
        "}\n");
    std::string text = error.has_value() ? *error : std::string("<no error>");
    expect(error.has_value() && text.find("argument 2 is 'int'") != std::string::npos,
           "overload_failure_names_the_offending_argument_and_types: expected the second argument to be named with "
           "its actual type, got '" + text + "'");
    expect(error.has_value() && text.find("expects 'int64_t'") != std::string::npos,
           "overload_failure_names_the_offending_argument_and_types: expected the parameter type to be named, got '" +
              text + "'");
    expect(error.has_value() && text.find("static_cast<T>") != std::string::npos,
           "overload_failure_names_the_offending_argument_and_types: expected the diagnostic to name the fix that "
           "actually works, got '" + text + "'");
    expect(error.has_value() && text.find("candidate: sink(bool, bool)") != std::string::npos,
           "overload_failure_names_the_offending_argument_and_types: expected every candidate to be listed, got '" +
              text + "'");
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

void test_switch_with_default_allows_branch_local_moves_without_post_switch_use() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "void consume(std::unique_ptr<int> p) { return; }\n"
        "int main() {\n"
        "    std::unique_ptr<int> p = std::make_unique<int>(7);\n"
        "    int selector = 2;\n"
        "    switch (selector) {\n"
        "        case 1:\n"
        "            consume(std::move(p));\n"
        "            break;\n"
        "        default:\n"
        "            return 0;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    expect(!error.has_value(),
           "switch_with_default_allows_branch_local_moves_without_post_switch_use: expected movecheck to accept" +
               (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

void test_switch_with_default_rejects_post_switch_use_of_maybe_moved_value() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void consume(std::unique_ptr<int> p) { return; }\n"
               "int main() {\n"
               "    std::unique_ptr<int> p = std::make_unique<int>(7);\n"
               "    int selector = 2;\n"
               "    switch (selector) {\n"
               "        case 1:\n"
               "            consume(std::move(p));\n"
               "            break;\n"
               "        default:\n"
               "            break;\n"
               "    }\n"
               "    return *p;\n"
               "}\n"),
           "switch_with_default_rejects_post_switch_use_of_maybe_moved_value: expected movecheck rejection");
}

void test_lambda_by_reference_capture_preserves_const_reference_readonlyness() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int read_const(const int& value) {\n"
        "    auto reader = [&]() -> int { return value; };\n"
        "    return reader();\n"
        "}\n"
        "int main() {\n"
        "    int seed = 7;\n"
        "    return read_const(seed) - 7;\n"
        "}\n");
    expect(!error.has_value(),
           "lambda_by_reference_capture_preserves_const_reference_readonlyness: expected movecheck to accept" +
              (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

void test_lambda_by_reference_capture_still_rejects_mutation_through_const_reference() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int mutate_const(const int& value) {\n"
        "    auto writer = [&]() -> int {\n"
        "        value = 1;\n"
        "        return 0;\n"
        "    };\n"
        "    return writer();\n"
        "}\n");
    expect(error.has_value() &&
              error->find("cannot assign to this place: it is reached through a read-only (const) reference") !=
                  std::string::npos,
           "lambda_by_reference_capture_still_rejects_mutation_through_const_reference: expected const-reference "
           "mutation diagnostic, got '" +
              (error.has_value() ? *error : std::string("<no error>")) + "'");
}

// The two tests below exercise scpp::monomorphize_generics/scpp::check_moves's
// std::expected<void, DataflowError> API shape directly, without going
// through move_error_message's optional<string>-message convenience wrapper
// -- mirroring parser_test.cpp's test_parse_returns_engaged_expected_on_success/
// test_parse_returns_disengaged_expected_on_failure_without_throwing added
// when parser.cppm made this same exceptions -> std::expected transition.
void test_check_moves_returns_engaged_expected_on_success() {
    cases_run++;
    scpp::Program program = parse_with_std_imports(
        "int main() {\n"
        "    return 0;\n"
        "}\n");
    std::expected<void, scpp::DataflowError> monomorphize_result = scpp::monomorphize_generics(program);
    expect(monomorphize_result.has_value(),
           "check_moves_returns_engaged_expected_on_success: expected monomorphize_generics's has_value() to be true");
    std::expected<void, scpp::DataflowError> check_result = scpp::check_moves(program);
    expect(check_result.has_value(),
           "check_moves_returns_engaged_expected_on_success: expected check_moves's has_value() to be true");
}

void test_check_moves_returns_disengaged_expected_on_failure_without_throwing() {
    cases_run++;
    // No try/catch here at all -- if scpp::check_moves still threw instead
    // of returning std::expected, this call itself would already have
    // aborted the test binary before reaching any of the expect() calls
    // below, since nothing in this function catches exceptions.
    scpp::Program program = parse_with_std_imports(
        "int f(int* p) {\n"
        "    return *p;\n"
        "}\n"
        "int main() {\n"
        "    return 0;\n"
        "}\n");
    std::expected<void, scpp::DataflowError> monomorphize_result = scpp::monomorphize_generics(program);
    expect(monomorphize_result.has_value(),
           "check_moves_returns_disengaged_expected_on_failure_without_throwing: expected monomorphize_generics to "
           "succeed");
    std::expected<void, scpp::DataflowError> result = scpp::check_moves(program);
    expect(!result.has_value(),
           "check_moves_returns_disengaged_expected_on_failure_without_throwing: expected has_value() to be false");
    if (result.has_value()) return;
    const scpp::DataflowError& error = result.error();
    expect(error.loc.is_known(),
           "check_moves_returns_disengaged_expected_on_failure_without_throwing: expected a known error location");
    expect(std::string(error.what()).size() > 0,
           "check_moves_returns_disengaged_expected_on_failure_without_throwing: expected a non-empty diagnostic "
           "message");
}

// --- keying locals by declaration (mir.cppm's LocalId) ----------------
//
// Every case below has two declarations that share a spelling. Before
// locals were keyed by their own declaration, a single name-keyed entry
// held whichever type was lowered *last*, so the first declaration's
// region was analysed with the second's type. The must-reject cases
// pin the consequence that matters: losing reference-ness skips the
// borrow tracking entirely, so a genuine violation was accepted.

// The control: the same borrow violation with no namesake anywhere. If
// this ever stops being rejected, the paired cases below prove nothing.
void test_borrow_violation_without_a_namesake_is_rejected() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void f() {\n"
               "    {\n"
               "        std::string s{\"hello\"};\n"
               "        std::string& r = s;\n"
               "        std::string t = std::move(s);\n"
               "        std::size_t n = r.size() + t.size();\n"
               "        n = n + 1;\n"
               "    }\n"
               "}\n"),
           "borrow_violation_without_a_namesake_is_rejected: expected the move of a borrowed value to be rejected");
}

// The same program plus a later sibling scope that reuses `r` for an
// `int`. That later declaration used to overwrite the reference's type,
// and a non-reference is not tracked as a borrow at all -- so the move
// above was let through.
void test_sibling_scope_namesake_does_not_hide_a_borrow() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void f() {\n"
               "    {\n"
               "        std::string s{\"hello\"};\n"
               "        std::string& r = s;\n"
               "        std::string t = std::move(s);\n"
               "        std::size_t n = r.size() + t.size();\n"
               "        n = n + 1;\n"
               "    }\n"
               "    {\n"
               "        int r = 0;\n"
               "        r = r + 1;\n"
               "    }\n"
               "}\n"),
           "sibling_scope_namesake_does_not_hide_a_borrow: expected the move of a borrowed value to still be "
           "rejected when a later sibling scope reuses the reference's name");
}

void test_if_else_branch_namesake_does_not_hide_a_borrow() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void f(bool flag) {\n"
               "    if (flag) {\n"
               "        std::string s{\"hello\"};\n"
               "        std::string& r = s;\n"
               "        std::string t = std::move(s);\n"
               "        std::size_t n = r.size() + t.size();\n"
               "        n = n + 1;\n"
               "    } else {\n"
               "        int r = 0;\n"
               "        r = r + 1;\n"
               "    }\n"
               "}\n"),
           "if_else_branch_namesake_does_not_hide_a_borrow: expected the move of a borrowed value in the `if` "
           "branch to still be rejected when the `else` branch reuses the reference's name");
}

void test_switch_case_namesake_does_not_hide_a_borrow() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void f(int selector) {\n"
               "    switch (selector) {\n"
               "        case 0: {\n"
               "            std::string s{\"hello\"};\n"
               "            std::string& r = s;\n"
               "            std::string t = std::move(s);\n"
               "            std::size_t n = r.size() + t.size();\n"
               "            n = n + 1;\n"
               "            break;\n"
               "        }\n"
               "        default: {\n"
               "            int r = 0;\n"
               "            r = r + 1;\n"
               "            break;\n"
               "        }\n"
               "    }\n"
               "}\n"),
           "switch_case_namesake_does_not_hide_a_borrow: expected the move of a borrowed value in one case to "
           "still be rejected when a sibling case reuses the reference's name");
}

void test_loop_body_namesake_does_not_hide_a_borrow() {
    cases_run++;
    expect(throws_move_error(
               "import std;\n"
               "void f() {\n"
               "    int i = 0;\n"
               "    while (i < 1) {\n"
               "        std::string s{\"hello\"};\n"
               "        std::string& r = s;\n"
               "        std::string t = std::move(s);\n"
               "        std::size_t n = r.size() + t.size();\n"
               "        n = n + 1;\n"
               "        i = i + 1;\n"
               "    }\n"
               "    {\n"
               "        int r = 0;\n"
               "        r = r + 1;\n"
               "    }\n"
               "}\n"),
           "loop_body_namesake_does_not_hide_a_borrow: expected the move of a borrowed value inside the loop "
           "body to still be rejected when a later scope reuses the reference's name");
}

// A shadowing inner declaration used to overwrite the outer local's type
// outright, so the member access below was checked against the *other*
// class -- reported as a private-member violation naming a class the
// expression never mentions.
void test_shadowing_namesake_does_not_retype_an_outer_member_access() {
    cases_run++;
    std::optional<std::string> message = move_error_message(
        "class Small {\n"
        "    int b = 0;\n"
        "public:\n"
        "    int a = 0;\n"
        "    Small() {}\n"
        "    virtual ~Small() {}\n"
        "};\n"
        "class Big {\n"
        "public:\n"
        "    int b = 0;\n"
        "    Big() {}\n"
        "    virtual ~Big() {}\n"
        "};\n"
        "int f() {\n"
        "    {\n"
        "        Big v{};\n"
        "        int n = v.b;\n"
        "        n = n + 1;\n"
        "    }\n"
        "    {\n"
        "        Small v{};\n"
        "        int m = v.a;\n"
        "        m = m + 1;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    expect(!message.has_value(),
           "shadowing_namesake_does_not_retype_an_outer_member_access: expected a public field read on 'Big' to "
           "be accepted, got " +
               message.value_or(std::string()));
}

// The liveness axis. An inner shadow's scope exit used to reset the
// *outer* local's tracked state, because both shared one name-keyed
// entry -- so the outer read after the inner block was rejected as
// out-of-scope. Distinct declarations now have distinct ids, so the
// inner one's ScopeExit cannot touch the outer one.
void test_inner_shadow_scope_exit_keeps_the_outer_local_live() {
    cases_run++;
    expect(!throws_move_error(
               "int f() {\n"
               "    int x = 1;\n"
               "    {\n"
               "        int x = 2;\n"
               "        x = 3;\n"
               "    }\n"
               "    return x;\n"
               "}\n"),
           "inner_shadow_scope_exit_keeps_the_outer_local_live: expected the outer local to still be readable "
           "after the shadowing inner scope ends");
}

void test_inner_shadow_of_a_different_type_is_accepted() {
    cases_run++;
    expect(!throws_move_error(
               "import std;\n"
               "int f() {\n"
               "    int x = 1;\n"
               "    {\n"
               "        std::string x{\"inner\"};\n"
               "        std::size_t n = x.size();\n"
               "        n = n + 1;\n"
               "    }\n"
               "    return x;\n"
               "}\n"),
           "inner_shadow_of_a_different_type_is_accepted: expected an inner shadow of an unrelated type to be "
           "accepted and to leave the outer local's own type intact");
}

// A capture names an enclosing local, and two enclosing declarations can
// share a spelling; the capture must take the one visible at the lambda,
// not the last one declared anywhere in the function.
void test_lambda_capture_binds_to_the_visible_declaration() {
    cases_run++;
    expect(!throws_move_error(
               "import std;\n"
               "int f() {\n"
               "    int captured = 1;\n"
               "    auto closure = [captured]() { return captured; };\n"
               "    {\n"
               "        std::string captured{\"other\"};\n"
               "        std::size_t n = captured.size();\n"
               "        n = n + 1;\n"
               "    }\n"
               "    return closure();\n"
               "}\n"),
           "lambda_capture_binds_to_the_visible_declaration: expected the capture to take the 'int' declaration "
           "visible at the lambda, not a later same-named one");
}

// `auto` is refined after lowering, by writing the inferred type back
// onto the declaration. Two `auto` namesakes must therefore refine
// independently; a name-keyed write made the second overwrite the first.
void test_auto_namesakes_infer_independently() {
    cases_run++;
    expect(!throws_move_error(
               "import std;\n"
               "int f() {\n"
               "    {\n"
               "        auto value = 1;\n"
               "        value = value + 1;\n"
               "    }\n"
               "    {\n"
               "        auto value = std::string{\"text\"};\n"
               "        std::size_t n = value.size();\n"
               "        n = n + 1;\n"
               "    }\n"
               "    return 0;\n"
               "}\n"),
           "auto_namesakes_infer_independently: expected each 'auto' declaration to keep its own inferred type");
}

// Diagnostics are written for humans: a local is always printed by the
// name it was declared with, never by the LocalId it is keyed by.
void test_diagnostics_print_the_declared_name() {
    cases_run++;
    std::optional<std::string> message = move_error_message(
        "import std;\n"
        "void f() {\n"
        "    {\n"
        "        std::string s{\"hello\"};\n"
        "        std::string& r = s;\n"
        "        std::string t = std::move(s);\n"
        "        std::size_t n = r.size() + t.size();\n"
        "        n = n + 1;\n"
        "    }\n"
        "    {\n"
        "        int r = 0;\n"
        "        r = r + 1;\n"
        "    }\n"
        "}\n");
    expect(message.has_value(), "diagnostics_print_the_declared_name: expected the program to be rejected");
    if (!message.has_value()) return;
    expect(message->find("'s'") != std::string::npos,
           "diagnostics_print_the_declared_name: expected the moved local's declared name in " + *message);
}

// The out-of-scope diagnostic reconstructs a name rather than passing one
// through, so it gets its own pin.
void test_out_of_scope_diagnostic_prints_the_declared_name() {
    cases_run++;
    std::optional<std::string> message = move_error_message(
        "int f() {\n"
        "    {\n"
        "        int inner_only = 1;\n"
        "        inner_only = inner_only + 1;\n"
        "    }\n"
        "    return inner_only;\n"
        "}\n");
    expect(message.has_value(), "out_of_scope_diagnostic_prints_the_declared_name: expected the program to be rejected");
    if (!message.has_value()) return;
    expect(message->find("'inner_only'") != std::string::npos,
           "out_of_scope_diagnostic_prints_the_declared_name: expected the local's declared name in " + *message);
}

// ch06 §6: `nullptr_t` converts to pointer types, function pointer
// types, and class types declaring a constructor that takes it -- and
// to nothing else. The rejection is asserted here rather than in
// codegen_test because that harness does not run check_moves at all,
// and the *point* of this change is that the rejection moved into
// movecheck. Before it did, the check happened in codegen's
// `check_store_type`, which sees only lowered LLVM types and so
// reported the destination as one of several "distinct scalar types"
// and advised `static_cast<T>(...)`. Both halves were wrong:
// `nullptr_t` is not a scalar, and scpp rejects that cast too -- so a
// user following the advice hit a second error. A diagnostic that
// cannot be acted on is the defect being fixed, which is why this
// asserts on the message text and not merely on rejection.
void test_nullptr_cannot_initialize_a_non_pointer_and_says_why() {
    cases_run++;
    for (std::string_view destination : {"bool", "int", "char", "size_t", "double"}) {
        std::string source = "int main() {\n    " + std::string(destination) +
                             " not_a_pointer = nullptr;\n    return 0;\n}\n";
        std::optional<std::string> message = move_error_message(source);
        std::string label =
            "nullptr_cannot_initialize_a_non_pointer_and_says_why[" + std::string(destination) + "]";
        expect(message.has_value(), label + ": expected the initialization to be rejected");
        if (!message.has_value()) continue;
        expect(message->find("'nullptr_t'") != std::string::npos,
               label + ": expected the source type to be named in " + *message);
        expect(message->find("'" + std::string(destination) + "'") != std::string::npos,
               label + ": expected the destination type to be named in " + *message);
        expect(message->find("static_cast") == std::string::npos,
               label + ": expected no advice to use a cast scpp itself rejects, got " + *message);
    }
}

// The other side of the same rule: every destination `nullptr_t` *does*
// convert to must still be accepted, including the two that were
// outright broken before. A null function pointer was rejected with
// "expected a function or function pointer with matching signature"
// even though a function pointer is a pointer; and a class-typed
// destination has to reach ordinary constructor overload resolution
// rather than being pre-empted by the new scalar check, which is what
// makes `std::unique_ptr<T> p = nullptr;` work.
void test_nullptr_initializes_every_pointer_shaped_destination() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "import std;\n"
        "int identity(int value) {\n"
        "    return value;\n"
        "}\n"
        "int main() {\n"
        "    int* raw = nullptr;\n"
        "    const int* to_const = nullptr;\n"
        "    void* opaque = nullptr;\n"
        "    nullptr_t bare = nullptr;\n"
        "    std::nullptr_t qualified = nullptr;\n"
        "    int (*function_pointer)(int) = nullptr;\n"
        "    std::unique_ptr<int> owned = nullptr;\n"
        "    std::shared_ptr<int> shared = nullptr;\n"
        "    return 0;\n"
        "}\n");
    expect(!error.has_value(),
           "nullptr_initializes_every_pointer_shaped_destination: expected every destination to be "
           "accepted" +
               (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

// `nullptr` borrows nothing, so it can never dangle. That was already
// the behavior, but it was keyed on the *spelling* -- movecheck
// compared an Identifier expression's name against "nullptr", so a
// local actually named `nullptr` would have been indistinguishable.
// It is now keyed on the expression kind. Returning a reference
// parameter's pointee is the shape that makes the lifetime machinery
// run, so this proves the null literal survives it.
void test_nullptr_return_is_not_treated_as_a_borrow() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int* pick(int* candidate, bool use_it) {\n"
        "    if (use_it) {\n"
        "        return candidate;\n"
        "    }\n"
        "    return nullptr;\n"
        "}\n"
        "int main() {\n"
        "    int value = 1;\n"
        "    int* chosen = pick(&value, true);\n"
        "    return chosen == nullptr ? 1 : 0;\n"
        "}\n");
    expect(!error.has_value(),
           "nullptr_return_is_not_treated_as_a_borrow: expected the null return to introduce no "
           "lifetime constraint" +
               (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}


// spec §6: the diagnostic has to name both types and point at the cast
// that fixes it -- these conversions are rejected on purpose, so the
// message is the entire user-facing product of the rule.
void test_scalar_conversion_diagnostic_names_both_types_and_the_cast() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int main() {\n"
        "    int a = 1;\n"
        "    unsigned int b = a;\n"
        "    if (b == 1) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "scalar_conversion_diagnostic_names_both_types_and_the_cast: expected int -> unsigned int to be "
           "rejected");
    if (!error.has_value()) return;
    expect(error->find("'int'") != std::string::npos,
           "scalar_conversion_diagnostic_names_both_types_and_the_cast: expected the source type to be named, got: " +
               *error);
    expect(error->find("'unsigned int'") != std::string::npos,
           "scalar_conversion_diagnostic_names_both_types_and_the_cast: expected the target type to be named, got: " +
               *error);
    expect(error->find("static_cast<unsigned int>") != std::string::npos,
           "scalar_conversion_diagnostic_names_both_types_and_the_cast: expected the suggested cast to be spelled "
           "out, got: " +
               *error);
}

// The return boundary names the function, so a diagnostic reported from
// a deeply nested call site still says which one is at fault.
void test_scalar_conversion_return_diagnostic_names_the_function() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int32_t widen() {\n"
        "    int a = 1;\n"
        "    return a;\n"
        "}\n"
        "int main() {\n"
        "    if (widen() == 1) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "scalar_conversion_return_diagnostic_names_the_function: expected the int -> int32_t return to be "
           "rejected");
    if (!error.has_value()) return;
    expect(error->find("widen") != std::string::npos,
           "scalar_conversion_return_diagnostic_names_the_function: expected the offending function to be named, "
           "got: " +
               *error);
}

// An out-of-range literal gets its own diagnostic rather than the
// conversion one: a literal has no source type to name, so "cannot
// convert an int to int8_t" would misdescribe what is wrong.
void test_out_of_range_literal_reports_the_value_not_a_conversion() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int main() {\n"
        "    int8_t x = 300;\n"
        "    if (x == 0) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "out_of_range_literal_reports_the_value_not_a_conversion: expected 300 to be rejected for int8_t");
    if (!error.has_value()) return;
    expect(error->find("300") != std::string::npos,
           "out_of_range_literal_reports_the_value_not_a_conversion: expected the offending value to be quoted, "
           "got: " +
               *error);
    expect(error->find("out of range") != std::string::npos,
           "out_of_range_literal_reports_the_value_not_a_conversion: expected an out-of-range diagnostic, got: " +
               *error);
    expect(error->find("no implicit conversion") == std::string::npos,
           "out_of_range_literal_reports_the_value_not_a_conversion: a literal has no source type, so the "
           "conversion wording is wrong here, got: " +
               *error);
}

// spec §6: the rule is about the two types, so it has to reach every
// place a value can be written to -- not just the locals and globals
// the statement-level path happens to see. A member or subscript target
// is lowered to an opaque expression rather than a MIR assignment, so
// these two shapes used to be checked by neither the name rule nor the
// representation backstop (the LLVM types match for int8_t/uint8_t) and
// silently miscompiled.
void test_scalar_conversion_into_a_member_place_is_rejected() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "struct S { uint8_t u; };\n"
        "int main() {\n"
        "    S s{};\n"
        "    int8_t a = 1;\n"
        "    s.u = a;\n"
        "    if (s.u == 1) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "scalar_conversion_into_a_member_place_is_rejected: expected int8_t -> uint8_t through a field to be "
           "rejected");
    if (!error.has_value()) return;
    expect(error->find("'s.u'") != std::string::npos,
           "scalar_conversion_into_a_member_place_is_rejected: expected the diagnostic to name the field being "
           "written, got: " +
               *error);
    expect(error->find("static_cast<uint8_t>") != std::string::npos,
           "scalar_conversion_into_a_member_place_is_rejected: expected the suggested cast to be spelled out, "
           "got: " +
               *error);
}

// The subscript shape has no name of its own to report, so the place
// description has to be synthesised from the base -- an empty '' in the
// message (which is what the representation backstop produced before)
// tells the user nothing about which write is at fault.
void test_scalar_conversion_into_a_subscript_place_names_the_array() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "int main() {\n"
        "    int arr[2];\n"
        "    unsigned int u = 1;\n"
        "    arr[0] = u;\n"
        "    if (arr[0] == 1) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "scalar_conversion_into_a_subscript_place_names_the_array: expected unsigned int -> int through a "
           "subscript to be rejected");
    if (!error.has_value()) return;
    expect(error->find("arr[") != std::string::npos,
           "scalar_conversion_into_a_subscript_place_names_the_array: expected the diagnostic to name the array "
           "being written, got: " +
               *error);
    expect(error->find("''") == std::string::npos,
           "scalar_conversion_into_a_subscript_place_names_the_array: the place must never be reported as an "
           "empty name, got: " +
               *error);
}

// A bare type-parameter name is a placeholder, not a type: judging `U*`
// against `T*` compares spellings and rejects code that substitution
// makes identical. The answer is deferred to monomorphization, which
// still catches the genuine mismatches (see the case below).
void test_pointer_write_between_unsubstituted_type_parameters_is_allowed() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "template<typename U>\n"
        "U* pass_through(U* p) {\n"
        "    return p;\n"
        "}\n"
        "template<typename T>\n"
        "class Box {\n"
        "  private:\n"
        "    T* ptr_{};\n"
        "  public:\n"
        "    Box() { return; }\n"
        "    void set(T* raw) {\n"
        "        T* tmp = pass_through<T>(raw);\n"
        "        this->ptr_ = tmp;\n"
        "        return;\n"
        "    }\n"
        "    virtual ~Box() { return; }\n"
        "};\n"
        "struct A { int x; };\n"
        "int main() {\n"
        "    A a{};\n"
        "    Box<A> b{};\n"
        "    b.set(&a);\n"
        "    return 0;\n"
        "}\n");
    expect(!error.has_value(),
           "pointer_write_between_unsubstituted_type_parameters_is_allowed: expected U* -> T* inside an "
           "uninstantiated generic to be accepted" +
               (error.has_value() ? std::string(", got '") + *error + "'" : ""));
}

// Deferring is not the same as disabling: once T is substituted, both
// pointees are real types again and a genuine mismatch is still caught.
void test_pointer_write_wrong_after_substitution_is_still_rejected() {
    cases_run++;
    std::optional<std::string> error = move_error_message(
        "struct A { int x; };\n"
        "struct B { int y; };\n"
        "template<typename T>\n"
        "class Box {\n"
        "  private:\n"
        "    T* ptr_{};\n"
        "  public:\n"
        "    Box() { return; }\n"
        "    void set(B* b) {\n"
        "        this->ptr_ = b;\n"
        "        return;\n"
        "    }\n"
        "    virtual ~Box() { return; }\n"
        "};\n"
        "int main() {\n"
        "    B b{};\n"
        "    Box<A> box{};\n"
        "    box.set(&b);\n"
        "    return 0;\n"
        "}\n");
    expect(error.has_value(),
           "pointer_write_wrong_after_substitution_is_still_rejected: expected B* -> A* to be rejected once T "
           "is substituted");
}

} // namespace


// ---------------------------------------------------------------------
// The scalar type model's tripwire.
//
// ch06 §6 fixes scpp's scalar types at exactly twenty names, and
// `scpp::scalar_type_info` (src/compiler/ast.cppm) is the single place
// in the compiler that lists them -- every other answer about a scalar
// derives from it. That is what stops the phases drifting apart, as they
// repeatedly did: `char` was signed to six sites and unsigned to two
// until recently, and the width answers disagreed about `size_t`.
//
// One list can still be edited wrongly, so this table restates the model
// independently. Adding a scalar type, or changing one's category, width
// or signedness, now has to be a deliberate edit in two places that
// agree -- and every derived answer below (the predicates, the layout,
// the value range, the literal-range check) is checked to follow from
// the model rather than being spot-checked on its own.
struct ScalarExpectation {
    const char* name;
    scpp::ScalarCategory category;
    int bit_width;
    bool is_unsigned;
    bool is_pointer_sized;
};

constexpr ScalarExpectation kExpectedScalars[] = {
    {"bool", scpp::ScalarCategory::Bool, 8, true, false},
    {"char", scpp::ScalarCategory::Integral, 8, false, false},
    {"int8_t", scpp::ScalarCategory::Integral, 8, false, false},
    {"uint8_t", scpp::ScalarCategory::Integral, 8, true, false},
    {"int16_t", scpp::ScalarCategory::Integral, 16, false, false},
    {"uint16_t", scpp::ScalarCategory::Integral, 16, true, false},
    {"int", scpp::ScalarCategory::Integral, 32, false, false},
    {"int32_t", scpp::ScalarCategory::Integral, 32, false, false},
    {"unsigned int", scpp::ScalarCategory::Integral, 32, true, false},
    {"uint32_t", scpp::ScalarCategory::Integral, 32, true, false},
    {"long", scpp::ScalarCategory::Integral, 64, false, false},
    {"int64_t", scpp::ScalarCategory::Integral, 64, false, false},
    {"unsigned long", scpp::ScalarCategory::Integral, 64, true, false},
    {"uint64_t", scpp::ScalarCategory::Integral, 64, true, false},
    {"size_t", scpp::ScalarCategory::Integral, 64, true, true},
    {"ptrdiff_t", scpp::ScalarCategory::Integral, 64, false, true},
    {"float", scpp::ScalarCategory::Floating, 32, false, false},
    {"float32_t", scpp::ScalarCategory::Floating, 32, false, false},
    {"double", scpp::ScalarCategory::Floating, 64, false, false},
    {"float64_t", scpp::ScalarCategory::Floating, 64, false, false},
};

void test_scalar_model_lists_exactly_the_twenty_names() {
    cases_run++;
    expect(std::size(kExpectedScalars) == 20,
           "scalar_model: ch06 §6 defines exactly twenty scalar types");

    // Names that must NOT be scalars: C++ spellings scpp deliberately
    // does not have, the non-scalar builtins, and a user-defined name.
    // `TypeKind::Named` covers all of these, which is exactly why the
    // model checks a closed set rather than the type's own kind.
    const char* non_scalars[] = {"void",        "nullptr_t", "short",     "signed char", "unsigned char",
                                 "long long",   "unsigned",  "signed",    "wchar_t",     "int128_t",
                                 "float16_t",   "uint128_t", "intptr_t",  "std::string", "Widget"};
    for (const char* name : non_scalars) {
        expect(!scpp::is_scalar_type_name(name),
               std::string("scalar_model: '") + name + "' must not be a scalar type");
    }
}

void test_scalar_model_matches_expected_shape() {
    cases_run++;
    for (const ScalarExpectation& expected : kExpectedScalars) {
        std::optional<scpp::ScalarTypeInfo> info = scpp::scalar_type_info(expected.name);
        if (!info.has_value()) {
            expect(false, std::string("scalar_model: '") + expected.name + "' is missing from scalar_type_info");
            continue;
        }
        const std::string where = std::string("scalar_model: '") + expected.name + "' ";
        expect(info->category == expected.category, where + "category");
        expect(info->bit_width == expected.bit_width, where + "bit_width");
        expect(info->is_unsigned == expected.is_unsigned, where + "is_unsigned");
        expect(info->is_pointer_sized == expected.is_pointer_sized, where + "is_pointer_sized");
    }
}

void test_scalar_predicates_derive_from_the_model() {
    cases_run++;
    for (const ScalarExpectation& expected : kExpectedScalars) {
        const std::string where = std::string("scalar_model: '") + expected.name + "' ";
        bool integral = expected.category == scpp::ScalarCategory::Integral;
        bool floating = expected.category == scpp::ScalarCategory::Floating;

        expect(scpp::is_scalar_type_name(expected.name), where + "is a scalar");
        expect(scpp::is_integral_scalar_type_name(expected.name) == integral, where + "integral predicate");
        expect(scpp::is_float_scalar_type_name(expected.name) == floating, where + "floating predicate");

        // The two signedness questions differ only in domain: the
        // integral one drives icmp/sdiv/negation and so excludes `bool`,
        // while the widening one covers every scalar and so includes it
        // (an i8 holding 0 or 1 must zero-extend). Both read the model's
        // single is_unsigned field.
        expect(scpp::is_unsigned_scalar_type_name(expected.name) == (integral && expected.is_unsigned),
               where + "integral signedness");
        expect(scpp::scalar_widens_unsigned(expected.name) == expected.is_unsigned, where + "widening signedness");
    }
    expect(!scpp::is_unsigned_scalar_type_name("bool"), "scalar_model: bool is not an unsigned *integral* scalar");
    expect(scpp::scalar_widens_unsigned("bool"), "scalar_model: bool must zero-extend when widened");
    expect(!scpp::scalar_widens_unsigned("char"), "scalar_model: char must sign-extend when widened");
}

void test_scalar_width_and_layout_agree() {
    cases_run++;
    const int pointer_bits = scpp::host_pointer_bit_width();
    scpp::Program empty_program{};
    for (const ScalarExpectation& expected : kExpectedScalars) {
        const std::string where = std::string("scalar_model: '") + expected.name + "' ";
        int width = scpp::scalar_bit_width(expected.name, pointer_bits);
        expect(width == (expected.is_pointer_sized ? pointer_bits : expected.bit_width), where + "width");

        scpp::Type type{};
        type.kind = scpp::TypeKind::Named;
        type.name = expected.name;
        std::optional<scpp::TypeLayoutInfo> layout = scpp::layout_of_type(empty_program, type);
        if (!layout.has_value()) {
            expect(false, where + "has no layout");
            continue;
        }
        expect(layout->size_bytes == static_cast<std::uint64_t>(width) / 8, where + "layout size follows width");
    }
}

void test_scalar_value_ranges_follow_width_and_signedness() {
    cases_run++;
    const int pointer_bits = scpp::host_pointer_bit_width();
    for (const ScalarExpectation& expected : kExpectedScalars) {
        const std::string where = std::string("scalar_model: '") + expected.name + "' ";
        std::optional<scpp::ScalarValueRange> range = scpp::scalar_value_range(expected.name, pointer_bits);

        if (expected.category == scpp::ScalarCategory::Floating) {
            expect(!range.has_value(), where + "floating types have no integer range");
            continue;
        }
        if (!range.has_value()) {
            expect(false, where + "has no value range");
            continue;
        }
        if (expected.category == scpp::ScalarCategory::Bool) {
            expect(range->min_value == 0 && range->max_value == 1, where + "bool range is {0, 1}");
            continue;
        }

        int width = scpp::scalar_bit_width(expected.name, pointer_bits);
        if (expected.is_unsigned) {
            expect(range->min_value == 0, where + "unsigned range starts at 0");
            // A 64-bit unsigned type cannot state its real maximum in an
            // std::int64_t carrier, so it clamps to INT64_MAX.
            if (width < 64) {
                expect(range->max_value == (std::int64_t{1} << width) - 1, where + "unsigned maximum");
            } else {
                expect(range->max_value == std::numeric_limits<std::int64_t>::max(), where + "clamped unsigned maximum");
            }
        } else if (width < 64) {
            expect(range->min_value == -(std::int64_t{1} << (width - 1)), where + "signed minimum");
            expect(range->max_value == (std::int64_t{1} << (width - 1)) - 1, where + "signed maximum");
        } else {
            expect(range->min_value == std::numeric_limits<std::int64_t>::min(), where + "64-bit signed minimum");
            expect(range->max_value == std::numeric_limits<std::int64_t>::max(), where + "64-bit signed maximum");
        }

        // The literal-range check must agree with the range at both
        // edges, and reject just outside them -- except that a 64-bit
        // unsigned type accepts negatives, because a literal above
        // INT64_MAX has already wrapped by the time it arrives and
        // cannot be told apart from a genuinely negative one.
        expect(scpp::integer_literal_value_fits(range->min_value, expected.name, pointer_bits),
               where + "accepts its minimum");
        expect(scpp::integer_literal_value_fits(range->max_value, expected.name, pointer_bits),
               where + "accepts its maximum");
        if (width < 64) {
            expect(!scpp::integer_literal_value_fits(range->max_value + 1, expected.name, pointer_bits),
                   where + "rejects one above its maximum");
            expect(!scpp::integer_literal_value_fits(range->min_value - 1, expected.name, pointer_bits),
                   where + "rejects one below its minimum");
        } else if (expected.is_unsigned) {
            expect(scpp::integer_literal_value_fits(-1, expected.name, pointer_bits),
                   where + "accepts a wrapped 64-bit unsigned literal");
        }
    }
}

// `literal_adopts_type` is the single authority for ch06 §6's
// literal-adoption rule -- move checking and constant evaluation both
// consult it, and before they did, they disagreed: move checking
// accepted `int8_t v = 5;` while the constexpr evaluator rejected the
// same initialization as an int-to-int8_t assignment, so a program's
// validity depended on which layer looked at it. This restates the rule
// independently of the implementation, so a change that quietly moves
// one layer's answer has to move this table too.
scpp::Expr integer_literal_expr(std::int64_t value) {
    scpp::Expr expr{};
    expr.kind = scpp::ExprKind::IntegerLiteral;
    expr.int_value = value;
    return expr;
}

scpp::Expr negated_integer_literal_expr(std::int64_t magnitude) {
    scpp::Expr expr{};
    expr.kind = scpp::ExprKind::Unary;
    expr.unary_op = scpp::UnaryOp::Neg;
    expr.lhs = std::make_unique<scpp::Expr>(integer_literal_expr(magnitude));
    return expr;
}

scpp::Expr simple_literal_expr(scpp::ExprKind kind) {
    scpp::Expr expr{};
    expr.kind = kind;
    return expr;
}

void test_literal_adoption_covers_every_scalar_type() {
    cases_run++;
    const int pointer_bits = scpp::host_pointer_bit_width();
    const scpp::Expr five = integer_literal_expr(5);
    const scpp::Expr one_point_five = simple_literal_expr(scpp::ExprKind::FloatLiteral);
    const scpp::Expr yes = simple_literal_expr(scpp::ExprKind::BoolLiteral);
    const scpp::Expr letter = simple_literal_expr(scpp::ExprKind::CharLiteral);

    for (const ScalarExpectation& expected : kExpectedScalars) {
        const std::string where = std::string("literal_adoption: '") + expected.name + "' ";
        const scpp::Type type = scpp::named_type(expected.name);
        const bool is_bool = expected.category == scpp::ScalarCategory::Bool;
        const bool is_char = std::string_view{expected.name} == "char";
        const bool is_float = expected.category == scpp::ScalarCategory::Floating;

        // An integer literal names a value of every scalar type except
        // `bool` and `char`: `true`/`false` and `'A'` are how those are
        // written, so `bool b = 1;` and `char c = 65;` are conversions.
        expect(scpp::literal_adopts_type(five, type, pointer_bits) == !(is_bool || is_char),
               where + "integer literal adoption");
        expect(scpp::literal_adopts_type(one_point_five, type, pointer_bits) == is_float,
               where + "floating literal adoption");
        expect(scpp::literal_adopts_type(yes, type, pointer_bits) == is_bool, where + "bool literal adoption");
        expect(scpp::literal_adopts_type(letter, type, pointer_bits) == is_char, where + "char literal adoption");

        // A place spelled `T&` is the place `T`.
        scpp::Type reference_type{};
        reference_type.kind = scpp::TypeKind::Reference;
        reference_type.pointee = std::make_shared<scpp::Type>(type);

        expect(scpp::literal_adopts_type(five, reference_type, pointer_bits) ==
                   scpp::literal_adopts_type(five, type, pointer_bits),
               where + "a reference place asks the same question as its referent");

        if (is_bool || is_char || is_float) continue;

        // Adoption is bounded by the type's own range, and a negated
        // literal is still a literal -- `int8_t x = -128;` names an
        // int8_t rather than converting one from `int`.
        std::optional<scpp::ScalarValueRange> range = scpp::scalar_value_range(expected.name, pointer_bits);
        if (!range.has_value()) {
            expect(false, where + "integral type has no value range");
            continue;
        }
        expect(scpp::literal_adopts_type(integer_literal_expr(range->max_value), type, pointer_bits),
               where + "adopts its maximum");
        if (scpp::scalar_bit_width(expected.name, pointer_bits) < 64) {
            expect(!scpp::literal_adopts_type(integer_literal_expr(range->max_value + 1), type, pointer_bits),
                   where + "rejects one above its maximum");
        }
        if (!expected.is_unsigned) {
            expect(scpp::literal_adopts_type(negated_integer_literal_expr(-range->min_value), type, pointer_bits),
                   where + "adopts its negated minimum");
        }
    }

    // `nullptr` adopts pointer places and nothing else -- in particular
    // no integer type, so `i == nullptr` stays an error.
    const scpp::Expr null = simple_literal_expr(scpp::ExprKind::NullptrLiteral);
    scpp::Type pointer_type{};
    pointer_type.kind = scpp::TypeKind::Pointer;
    pointer_type.pointee = std::make_shared<scpp::Type>(scpp::named_type("int"));
    expect(scpp::literal_adopts_type(null, pointer_type, scpp::host_pointer_bit_width()),
           "literal_adoption: nullptr adopts a pointer place");
    expect(!scpp::literal_adopts_type(null, scpp::named_type("int"), scpp::host_pointer_bit_width()),
           "literal_adoption: nullptr does not adopt an integer place");
    expect(!scpp::literal_adopts_type(null, scpp::named_type("bool"), scpp::host_pointer_bit_width()),
           "literal_adoption: nullptr does not adopt a bool place");
}

int main() {
    run_test_case_files();
    test_literal_adoption_covers_every_scalar_type();
    test_scalar_model_lists_exactly_the_twenty_names();
    test_scalar_model_matches_expected_shape();
    test_scalar_predicates_derive_from_the_model();
    test_scalar_width_and_layout_agree();
    test_scalar_value_ranges_follow_width_and_signedness();
    test_mutable_reborrow_is_allowed_while_nested();
    test_mutable_reborrow_allows_parent_read_while_live();
    test_mutable_reborrow_rejects_parent_write_while_live();
    test_mutable_reborrow_parent_becomes_usable_after_scope();
    test_range_for_const_reference_rejects_mutation();
    test_range_for_mutable_reference_over_span_is_accepted();
    test_range_for_const_reference_over_span_rejects_mutation();
    test_non_const_method_call_through_const_reference_reports_clear_diagnostic();
    test_shared_reference_can_be_lent_on_to_a_const_parameter();
    test_shared_alias_of_a_mutable_reference_can_be_lent_on();
    test_shared_reference_still_cannot_satisfy_a_mutable_parameter();
    test_read_only_range_for_still_allows_reading_the_container();
    test_by_value_range_for_over_a_member_accessor_allows_other_member_borrows();
    test_mutable_range_for_still_borrows_the_container_exclusively();
    test_read_only_range_for_still_rejects_mutation_through_the_loop_variable();
    test_read_only_range_for_survives_generic_instantiation();
    test_converting_constructor_is_accepted_as_a_call_argument();
    test_converting_constructor_is_accepted_as_a_return_operand();
    test_converting_constructor_is_accepted_as_a_variable_initializer();
    test_converting_constructor_is_accepted_as_a_constructor_argument();
    test_converting_constructor_is_selected_among_constructor_overloads();
    test_same_type_initializer_still_requires_copy_constructibility();
    test_expected_of_a_smart_pointer_accepts_nullptr();
    test_rejected_initializer_diagnostic_advises_a_syntax_that_parses();
    test_move_of_a_member_reports_the_same_reason_at_every_boundary();
    test_overload_failure_distinguishes_arity_from_argument_type();
    test_overload_failure_names_the_offending_argument_and_types();
    test_std_string_const_reference_mutation_reports_clear_diagnostic();
    test_derived_constructor_requires_explicit_base_initializer_without_default_base_ctor();
    test_explicit_base_initializer_satisfies_nondefault_base_ctor();
    test_switch_with_default_allows_branch_local_moves_without_post_switch_use();
    test_switch_with_default_rejects_post_switch_use_of_maybe_moved_value();
    test_lambda_by_reference_capture_preserves_const_reference_readonlyness();
    test_lambda_by_reference_capture_still_rejects_mutation_through_const_reference();
    test_check_moves_returns_engaged_expected_on_success();
    test_check_moves_returns_disengaged_expected_on_failure_without_throwing();

    test_borrow_violation_without_a_namesake_is_rejected();
    test_sibling_scope_namesake_does_not_hide_a_borrow();
    test_if_else_branch_namesake_does_not_hide_a_borrow();
    test_switch_case_namesake_does_not_hide_a_borrow();
    test_loop_body_namesake_does_not_hide_a_borrow();
    test_shadowing_namesake_does_not_retype_an_outer_member_access();
    test_inner_shadow_scope_exit_keeps_the_outer_local_live();
    test_inner_shadow_of_a_different_type_is_accepted();
    test_lambda_capture_binds_to_the_visible_declaration();
    test_auto_namesakes_infer_independently();
    test_diagnostics_print_the_declared_name();
    test_out_of_scope_diagnostic_prints_the_declared_name();
    test_nullptr_cannot_initialize_a_non_pointer_and_says_why();
    test_nullptr_initializes_every_pointer_shaped_destination();
    test_nullptr_return_is_not_treated_as_a_borrow();

    test_scalar_conversion_diagnostic_names_both_types_and_the_cast();
    test_scalar_conversion_return_diagnostic_names_the_function();
    test_scalar_conversion_into_a_member_place_is_rejected();
    test_scalar_conversion_into_a_subscript_place_names_the_array();
    test_pointer_write_between_unsubstituted_type_parameters_is_allowed();
    test_pointer_write_wrong_after_substitution_is_still_rejected();
    test_out_of_range_literal_reports_the_value_not_a_conversion();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All move-check tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
