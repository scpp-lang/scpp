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
    test_shared_reference_can_be_lent_on_to_a_const_parameter();
    test_shared_alias_of_a_mutable_reference_can_be_lent_on();
    test_shared_reference_still_cannot_satisfy_a_mutable_parameter();
    test_read_only_range_for_still_allows_reading_the_container();
    test_by_value_range_for_over_a_member_accessor_allows_other_member_borrows();
    test_mutable_range_for_still_borrows_the_container_exclusively();
    test_read_only_range_for_still_rejects_mutation_through_the_loop_variable();
    test_read_only_range_for_survives_generic_instantiation();
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

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All move-check tests passed (" << cases_run << " case file(s)).\n";
    return 0;
}
