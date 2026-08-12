import scpp.parser;
import scpp.ast;
import std;

#ifndef SCPP_STDLIB_STD_MODULE_PATH
#error "SCPP_STDLIB_STD_MODULE_PATH must be defined by the build"
#endif

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        failures++;
    }
}

bool is_named_type(const scpp::Type& type, std::string_view name) {
    return type.kind == scpp::TypeKind::Named && type.name == name;
}

const scpp::Function* find_function_named(const scpp::Program& program, std::string_view name) {
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

const scpp::ClassDef* find_class_named(const scpp::Program& program, std::string_view name) {
    for (const scpp::ClassDef& def : program.classes) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

const scpp::StructDef* find_struct_named(const scpp::Program& program, std::string_view name) {
    for (const scpp::StructDef& def : program.structs) {
        if (def.name == name) return &def;
    }
    return nullptr;
}

const scpp::TypeAliasDecl* find_type_alias_named(const scpp::Program& program, std::string_view name) {
    for (const scpp::TypeAliasDecl& alias : program.type_aliases) {
        if (alias.name == name) return &alias;
    }
    return nullptr;
}

std::string read_file(const std::string& path) {
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

// Test-only convenience for the overwhelmingly common "this source is
// expected to parse successfully" case: unwraps scpp::parse's
// std::expected result, reporting a clear test failure (via expect(),
// this file's own existing failure-reporting idiom) with the real
// ParseError message if parsing unexpectedly fails, rather than letting
// callers repeat a has_value() check at every one of this file's own
// call sites. Only ever used for happy-path parses -- a test that
// expects parsing to *fail* checks scpp::parse's result directly (see
// e.g. test_bare_local_var_decl_is_rejected above). Forwards every
// optional parameter scpp::parse itself accepts, since a handful of
// callers below need a partition_resolver (ch11 §11.4 coverage).
scpp::Program expect_parse_ok(std::string_view source, const scpp::ModuleResolver& resolver = {},
                              const scpp::PartitionResolver& partition_resolver = {}, std::string source_path = {}) {
    auto result = scpp::parse(source, resolver, partition_resolver, std::move(source_path));
    if (!result.has_value()) {
        throw std::runtime_error(std::string("scpp::parse unexpectedly failed: ") + result.error().what());
    }
    return std::move(result.value());
}

void test_int_main_return() {
    scpp::Program program = expect_parse_ok("int main() { return 42; }");
    expect(program.functions.size() == 1, "int_main_return: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(is_named_type(fn.return_type, "int"), "int_main_return: return type should be 'int'");
    expect(fn.name == "main", "int_main_return: name should be 'main'");
    expect(fn.params.empty(), "int_main_return: no params expected");
    expect(fn.body->kind == scpp::StmtKind::Block, "int_main_return: body should be a block");
    expect(fn.body->statements.size() == 1, "int_main_return: block should have 1 statement");

    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return, "int_main_return: statement should be Return");
    expect(ret.expr != nullptr, "int_main_return: return should have a value");
    expect(ret.expr->kind == scpp::ExprKind::IntegerLiteral, "int_main_return: value should be IntegerLiteral");
    expect(ret.expr->int_value == 42, "int_main_return: value should be 42");
}

void test_function_with_params() {
    scpp::Program program = expect_parse_ok("int add(int a, int b) { return a + b; }");
    expect(program.functions.size() == 1, "function_with_params: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 2, "function_with_params: expected 2 params");
    expect(is_named_type(fn.params[0].type, "int") && fn.params[0].name == "a",
           "function_with_params: param 0 should be 'int a'");
    expect(is_named_type(fn.params[1].type, "int") && fn.params[1].name == "b",
           "function_with_params: param 1 should be 'int b'");

    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.expr->kind == scpp::ExprKind::Binary, "function_with_params: expr should be Binary");
    expect(ret.expr->binary_op == scpp::BinaryOp::Add, "function_with_params: op should be Add");
    expect(ret.expr->lhs->kind == scpp::ExprKind::Identifier && ret.expr->lhs->name == "a",
           "function_with_params: lhs should be identifier 'a'");
    expect(ret.expr->rhs->kind == scpp::ExprKind::Identifier && ret.expr->rhs->name == "b",
           "function_with_params: rhs should be identifier 'b'");
}

void test_var_decl_and_if_else() {
    scpp::Program program = expect_parse_ok(
        "int f() {"
        "    int x = 1;"
        "    if (x < 2) { return 1; } else { return 0; }"
        "}");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 2, "var_decl_and_if_else: expected 2 statements");

    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "var_decl_and_if_else: statement 0 should be VarDecl");
    expect(is_named_type(decl.type, "int") && decl.var_name == "x",
           "var_decl_and_if_else: decl should be 'int x'");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::IntegerLiteral &&
               decl.init->int_value == 1,
           "var_decl_and_if_else: init should be IntegerLiteral 1");

    const scpp::Stmt& if_stmt = *fn.body->statements[1];
    expect(if_stmt.kind == scpp::StmtKind::If, "var_decl_and_if_else: statement 1 should be If");
    expect(if_stmt.condition->kind == scpp::ExprKind::Binary && if_stmt.condition->binary_op == scpp::BinaryOp::Lt,
           "var_decl_and_if_else: condition should be Lt");
    expect(if_stmt.then_branch != nullptr, "var_decl_and_if_else: then_branch should be present");
    expect(if_stmt.else_branch != nullptr, "var_decl_and_if_else: else_branch should be present");
}

void test_class_var_decl_with_brace_init_parses_ctor_args() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "public:\n"
        "    Box(int value) {}\n"
        "};\n"
        "int main() {\n"
        "    Box box{42};\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "class_var_decl_with_brace_init_parses_ctor_args: expected main");
    if (main_fn == nullptr) return;
    expect(main_fn->body->statements.size() == 2,
           "class_var_decl_with_brace_init_parses_ctor_args: expected 2 statements in main");
    const scpp::Stmt& decl = *main_fn->body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl,
           "class_var_decl_with_brace_init_parses_ctor_args: first statement should be VarDecl");
    expect(is_named_type(decl.type, "Box") && decl.var_name == "box",
           "class_var_decl_with_brace_init_parses_ctor_args: decl should be 'Box box'");
    expect(decl.has_ctor_args, "class_var_decl_with_brace_init_parses_ctor_args: expected ctor args");
    expect(decl.ctor_args.size() == 1,
           "class_var_decl_with_brace_init_parses_ctor_args: expected exactly 1 ctor arg");
    if (decl.ctor_args.size() == 1) {
        expect(decl.ctor_args[0]->kind == scpp::ExprKind::IntegerLiteral && decl.ctor_args[0]->int_value == 42,
               "class_var_decl_with_brace_init_parses_ctor_args: ctor arg should be IntegerLiteral 42");
    }
}

void test_class_var_decl_with_paren_init_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "class Box {\n"
            "public:\n"
            "    Box(int value) {}\n"
            "};\n"
            "int main() {\n"
            "    Box box(42);\n"
            "    return 0;\n"
            "}\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("use brace-init instead") != std::string::npos,
               "class_var_decl_with_paren_init_is_rejected: expected brace-init guidance in error message");
    }
    expect(threw, "class_var_decl_with_paren_init_is_rejected: expected a ParseError");
}

void test_bare_local_var_decl_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int main() { int x; return 0; }"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("explicit initializer") != std::string::npos,
               "bare_local_var_decl_is_rejected: expected explicit-initializer guidance");
    }
    expect(threw, "bare_local_var_decl_is_rejected: expected a ParseError");
}

void test_static_local_var_decl_parses_and_allows_no_initializer() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int f() {\n"
        "    static const std::string empty;\n"
        "    return (int)empty.size();\n"
        "}\n");
    const scpp::Function* fn = find_function_named(program, "f");
    expect(fn != nullptr, "static_local_var_decl_parses_and_allows_no_initializer: expected f");
    if (fn == nullptr) return;
    expect(fn->body->statements.size() == 2,
           "static_local_var_decl_parses_and_allows_no_initializer: expected 2 statements");
    if (fn->body->statements.size() != 2) return;
    const scpp::Stmt& decl = *fn->body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl,
           "static_local_var_decl_parses_and_allows_no_initializer: first statement should be VarDecl");
    expect(decl.is_static_local,
           "static_local_var_decl_parses_and_allows_no_initializer: local should be marked static");
    expect(decl.is_const,
           "static_local_var_decl_parses_and_allows_no_initializer: local should be marked const");
    expect(is_named_type(decl.type, "std::string"),
           "static_local_var_decl_parses_and_allows_no_initializer: type should be std::string");
    expect(decl.init == nullptr && !decl.has_ctor_args,
           "static_local_var_decl_parses_and_allows_no_initializer: no explicit initializer should be preserved");
}

void test_fixed_width_integer_keywords_and_std_qualification_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Box {};\n"
        "struct Pair {\n"
        "    std::int64_t lhs;\n"
        "    uint32_t rhs;\n"
        "};\n"
        "int64_t add(std::int64_t lhs, std::uint32_t rhs) {\n"
        "    Box<std::int64_t> a{};\n"
        "    Box<int64_t> b{};\n"
        "    std::int64_t x = lhs;\n"
        "    uint32_t y = rhs;\n"
        "    return x + (std::int64_t)y;\n"
        "}\n");
    const scpp::StructDef* pair = find_struct_named(program, "Pair");
    expect(pair != nullptr, "fixed_width_integer_keywords_and_std_qualification_parse: expected Pair");
    if (pair == nullptr) return;
    expect(pair->fields.size() == 2, "fixed_width_integer_keywords_and_std_qualification_parse: expected 2 Pair fields");
    if (pair->fields.size() == 2) {
        expect(is_named_type(pair->fields[0].type, "int64_t"),
               "fixed_width_integer_keywords_and_std_qualification_parse: lhs should canonicalize to int64_t");
        expect(is_named_type(pair->fields[1].type, "uint32_t"),
               "fixed_width_integer_keywords_and_std_qualification_parse: rhs should canonicalize to uint32_t");
    }
    const scpp::Function* add = find_function_named(program, "add");
    expect(add != nullptr, "fixed_width_integer_keywords_and_std_qualification_parse: expected add");
    if (add == nullptr) return;
    expect(is_named_type(add->return_type, "int64_t"),
           "fixed_width_integer_keywords_and_std_qualification_parse: return type should canonicalize to int64_t");
    expect(add->params.size() == 2, "fixed_width_integer_keywords_and_std_qualification_parse: expected 2 params");
    if (add->params.size() == 2) {
        expect(is_named_type(add->params[0].type, "int64_t"),
               "fixed_width_integer_keywords_and_std_qualification_parse: param lhs should canonicalize to int64_t");
        expect(is_named_type(add->params[1].type, "uint32_t"),
               "fixed_width_integer_keywords_and_std_qualification_parse: param rhs should canonicalize to uint32_t");
    }
    expect(add->body != nullptr && add->body->statements.size() == 5,
           "fixed_width_integer_keywords_and_std_qualification_parse: expected 5 statements");
    if (add->body == nullptr || add->body->statements.size() != 5) return;
    const scpp::Stmt& a_decl = *add->body->statements[0];
    const scpp::Stmt& b_decl = *add->body->statements[1];
    const scpp::Stmt& x_decl = *add->body->statements[2];
    const scpp::Stmt& y_decl = *add->body->statements[3];
    expect(a_decl.kind == scpp::StmtKind::VarDecl && a_decl.type.template_args.size() == 1 &&
               a_decl.type.name == "Box" && is_named_type(a_decl.type.template_args[0], "int64_t"),
           "fixed_width_integer_keywords_and_std_qualification_parse: Box<std::int64_t> should canonicalize to Box<int64_t>");
    expect(b_decl.kind == scpp::StmtKind::VarDecl && b_decl.type.template_args.size() == 1 &&
               b_decl.type.name == "Box" && is_named_type(b_decl.type.template_args[0], "int64_t"),
           "fixed_width_integer_keywords_and_std_qualification_parse: Box<int64_t> should stay Box<int64_t>");
    expect(x_decl.kind == scpp::StmtKind::VarDecl && is_named_type(x_decl.type, "int64_t"),
           "fixed_width_integer_keywords_and_std_qualification_parse: local x should canonicalize to int64_t");
    expect(y_decl.kind == scpp::StmtKind::VarDecl && is_named_type(y_decl.type, "uint32_t"),
           "fixed_width_integer_keywords_and_std_qualification_parse: local y should canonicalize to uint32_t");
}

void test_size_t_and_ptrdiff_t_keywords_and_std_qualification_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Box {};\n"
        "struct Pair {\n"
        "    std::size_t count;\n"
        "    ptrdiff_t delta;\n"
        "};\n"
        "size_t add(std::size_t count, std::ptrdiff_t delta) {\n"
        "    Box<std::size_t> a{};\n"
        "    Box<ptrdiff_t> b{};\n"
        "    size_t x = count;\n"
        "    ptrdiff_t y = delta;\n"
        "    return x + (size_t)y;\n"
        "}\n");
    const scpp::StructDef* pair = find_struct_named(program, "Pair");
    expect(pair != nullptr, "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: expected Pair");
    if (pair == nullptr) return;
    expect(pair->fields.size() == 2,
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: expected 2 Pair fields");
    if (pair->fields.size() == 2) {
        expect(is_named_type(pair->fields[0].type, "size_t"),
               "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: count should canonicalize to size_t");
        expect(is_named_type(pair->fields[1].type, "ptrdiff_t"),
               "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: delta should canonicalize to ptrdiff_t");
    }
    const scpp::Function* add = find_function_named(program, "add");
    expect(add != nullptr, "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: expected add");
    if (add == nullptr) return;
    expect(is_named_type(add->return_type, "size_t"),
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: return type should canonicalize to size_t");
    expect(add->params.size() == 2, "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: expected 2 params");
    if (add->params.size() == 2) {
        expect(is_named_type(add->params[0].type, "size_t"),
               "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: param count should canonicalize to size_t");
        expect(is_named_type(add->params[1].type, "ptrdiff_t"),
               "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: param delta should canonicalize to ptrdiff_t");
    }
    expect(add->body != nullptr && add->body->statements.size() == 5,
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: expected 5 statements");
    if (add->body == nullptr || add->body->statements.size() != 5) return;
    const scpp::Stmt& a_decl = *add->body->statements[0];
    const scpp::Stmt& b_decl = *add->body->statements[1];
    const scpp::Stmt& x_decl = *add->body->statements[2];
    const scpp::Stmt& y_decl = *add->body->statements[3];
    expect(a_decl.kind == scpp::StmtKind::VarDecl && a_decl.type.template_args.size() == 1 &&
               a_decl.type.name == "Box" && is_named_type(a_decl.type.template_args[0], "size_t"),
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: Box<std::size_t> should canonicalize to Box<size_t>");
    expect(b_decl.kind == scpp::StmtKind::VarDecl && b_decl.type.template_args.size() == 1 &&
               b_decl.type.name == "Box" && is_named_type(b_decl.type.template_args[0], "ptrdiff_t"),
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: Box<ptrdiff_t> should stay Box<ptrdiff_t>");
    expect(x_decl.kind == scpp::StmtKind::VarDecl && is_named_type(x_decl.type, "size_t"),
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: local x should canonicalize to size_t");
    expect(y_decl.kind == scpp::StmtKind::VarDecl && is_named_type(y_decl.type, "ptrdiff_t"),
           "size_t_and_ptrdiff_t_keywords_and_std_qualification_parse: local y should canonicalize to ptrdiff_t");
}

void test_valid_local_initializer_forms_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Pair { int first; int second; };\n"
        "int main() {\n"
        "    int a{};\n"
        "    Pair b{1, 2};\n"
        "    int c = 3;\n"
        "    return a + b.first + b.second + c;\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "valid_local_initializer_forms_parse: expected main");
    if (main_fn == nullptr) return;
    expect(main_fn->body->statements.size() == 4, "valid_local_initializer_forms_parse: expected 4 statements");
    if (main_fn->body->statements.size() != 4) return;

    const scpp::Stmt& empty_braces = *main_fn->body->statements[0];
    expect(empty_braces.kind == scpp::StmtKind::VarDecl && empty_braces.has_ctor_args && empty_braces.ctor_args.empty(),
           "valid_local_initializer_forms_parse: int a{} should parse as brace-init");

    const scpp::Stmt& brace_args = *main_fn->body->statements[1];
    expect(brace_args.kind == scpp::StmtKind::VarDecl && brace_args.has_ctor_args && brace_args.ctor_args.size() == 2,
           "valid_local_initializer_forms_parse: Pair b{1, 2} should keep 2 brace args");

    const scpp::Stmt& equals_init = *main_fn->body->statements[2];
    expect(equals_init.kind == scpp::StmtKind::VarDecl && equals_init.init != nullptr &&
               equals_init.init->kind == scpp::ExprKind::IntegerLiteral && equals_init.init->int_value == 3,
           "valid_local_initializer_forms_parse: int c = 3 should keep '=' initializer");
}

void test_class_default_member_initializers_parse() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "    int a{};\n"
        "    int b = 7;\n"
        "public:\n"
        "    Box() {}\n"
        "};\n");
    const scpp::ClassDef* box = find_class_named(program, "Box");
    expect(box != nullptr, "class_default_member_initializers_parse: expected Box");
    if (box == nullptr) return;
    expect(box->fields.size() == 2, "class_default_member_initializers_parse: expected 2 fields");
    if (box->fields.size() != 2) return;

    expect(box->fields[0].default_initializer.has_value(),
           "class_default_member_initializers_parse: first field should have brace default");
    if (box->fields[0].default_initializer.has_value()) {
        expect(box->fields[0].default_initializer->has_brace_args &&
                   box->fields[0].default_initializer->brace_args.empty(),
               "class_default_member_initializers_parse: int a{} should preserve empty brace-init");
    }

    expect(box->fields[1].default_initializer.has_value(),
           "class_default_member_initializers_parse: second field should have '=' default");
    if (box->fields[1].default_initializer.has_value()) {
        expect(!box->fields[1].default_initializer->has_brace_args &&
                   box->fields[1].default_initializer->expr != nullptr &&
                   box->fields[1].default_initializer->expr->kind == scpp::ExprKind::IntegerLiteral &&
                   box->fields[1].default_initializer->expr->int_value == 7,
               "class_default_member_initializers_parse: int b = 7 should preserve expression initializer");
    }
}

void test_constructor_member_initializer_list_parses() {
    scpp::Program program = expect_parse_ok(
        "class Holder {\n"
        "    int value;\n"
        "    int& ref;\n"
        "public:\n"
        "    Holder(int seed, int& input) : ref{input}, value{seed} {}\n"
        "};\n");
    const scpp::Function* ctor = find_function_named(program, "Holder_new");
    expect(ctor != nullptr, "constructor_member_initializer_list_parses: expected constructor");
    if (ctor == nullptr) return;
    expect(ctor->member_initializers.size() == 2,
           "constructor_member_initializer_list_parses: expected 2 member initializers");
    if (ctor->member_initializers.size() != 2) return;

    expect(ctor->member_initializers[0].member_name == "ref" &&
               ctor->member_initializers[0].initializer.has_brace_args &&
               ctor->member_initializers[0].initializer.brace_args.size() == 1,
           "constructor_member_initializer_list_parses: ref{input} should be preserved");
    expect(ctor->member_initializers[1].member_name == "value" &&
               ctor->member_initializers[1].initializer.has_brace_args &&
               ctor->member_initializers[1].initializer.brace_args.size() == 1,
           "constructor_member_initializer_list_parses: value{seed} should be preserved");
}

void test_constructor_base_initializer_list_parses() {
    scpp::Program program = expect_parse_ok(
        "class Base {\n"
        "public:\n"
        "    Base(int seed) { return; }\n"
        "};\n"
        "class Derived : public Base {\n"
        "public:\n"
        "    Derived(int seed) : Base{seed} { return; }\n"
        "};\n");
    const scpp::Function* ctor = find_function_named(program, "Derived_new");
    expect(ctor != nullptr, "constructor_base_initializer_list_parses: expected constructor");
    if (ctor == nullptr) return;
    expect(ctor->member_initializers.size() == 1,
           "constructor_base_initializer_list_parses: expected 1 initializer");
    if (ctor->member_initializers.size() != 1) return;
    expect(ctor->member_initializers[0].member_name == "Base" &&
               ctor->member_initializers[0].initializer.has_brace_args &&
               ctor->member_initializers[0].initializer.brace_args.size() == 1,
           "constructor_base_initializer_list_parses: Base{seed} should be preserved");
}

void test_while_loop() {
    scpp::Program program = expect_parse_ok("int f() { while (true) { x = x - 1; } }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& while_stmt = *fn.body->statements[0];
    expect(while_stmt.kind == scpp::StmtKind::While, "while_loop: statement should be While");
    expect(while_stmt.condition->kind == scpp::ExprKind::BoolLiteral && while_stmt.condition->bool_value,
           "while_loop: condition should be BoolLiteral true");
    expect(while_stmt.then_branch->kind == scpp::StmtKind::Block, "while_loop: body should be a block");

    const scpp::Stmt& assign_stmt = *while_stmt.then_branch->statements[0];
    expect(assign_stmt.kind == scpp::StmtKind::ExprStmt, "while_loop: body statement should be ExprStmt");
    expect(assign_stmt.expr->kind == scpp::ExprKind::Binary && assign_stmt.expr->binary_op == scpp::BinaryOp::Assign,
           "while_loop: expr should be an Assign");
}

void test_classic_for_loop_desugars_with_scoped_init() {
    scpp::Program program = expect_parse_ok(
        "int main() { for (int i = 0; i < 3; i = i + 1) { x = i; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 2, "classic_for_loop_desugars_with_scoped_init: expected loop + return");
    const scpp::Stmt& outer_block = *fn.body->statements[0];
    expect(outer_block.kind == scpp::StmtKind::Block,
           "classic_for_loop_desugars_with_scoped_init: loop should desugar to a block");
    expect(outer_block.statements.size() == 2,
           "classic_for_loop_desugars_with_scoped_init: expected init decl + while");
    if (outer_block.statements.size() != 2) return;
    expect(outer_block.statements[0]->kind == scpp::StmtKind::VarDecl &&
               outer_block.statements[0]->var_name == "i",
           "classic_for_loop_desugars_with_scoped_init: first block stmt should declare i");
    const scpp::Stmt& while_stmt = *outer_block.statements[1];
    expect(while_stmt.kind == scpp::StmtKind::While,
           "classic_for_loop_desugars_with_scoped_init: second block stmt should be While");
    expect(while_stmt.then_branch != nullptr && while_stmt.then_branch->kind == scpp::StmtKind::Block,
           "classic_for_loop_desugars_with_scoped_init: while body should be a block");
    if (while_stmt.then_branch == nullptr || while_stmt.then_branch->statements.size() != 2) return;
    expect(while_stmt.then_branch->statements[1]->kind == scpp::StmtKind::ExprStmt &&
               while_stmt.then_branch->statements[1]->expr != nullptr &&
               while_stmt.then_branch->statements[1]->expr->kind == scpp::ExprKind::Binary &&
               while_stmt.then_branch->statements[1]->expr->binary_op == scpp::BinaryOp::Assign,
           "classic_for_loop_desugars_with_scoped_init: while body should end with increment assignment");
}

void test_classic_for_loop_with_expression_init_desugars() {
    scpp::Program program = expect_parse_ok(
        "int main() { int i = 5; for (i = 3; i > 0; i = i - 1) i = i - 1; return i; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 3,
           "classic_for_loop_with_expression_init_desugars: expected decl + loop + return");
    const scpp::Stmt& outer_block = *fn.body->statements[1];
    expect(outer_block.kind == scpp::StmtKind::Block,
           "classic_for_loop_with_expression_init_desugars: loop should desugar to a block");
    expect(outer_block.statements.size() == 2,
           "classic_for_loop_with_expression_init_desugars: expected init expr + while");
    if (outer_block.statements.size() != 2) return;
    expect(outer_block.statements[0]->kind == scpp::StmtKind::ExprStmt,
           "classic_for_loop_with_expression_init_desugars: first block stmt should be init expr");
    expect(outer_block.statements[1]->kind == scpp::StmtKind::While,
           "classic_for_loop_with_expression_init_desugars: second block stmt should be While");
}

void test_range_for_loop_desugars_over_array() {
    scpp::Program program = expect_parse_ok(
        "int main() { int values[3]; for (auto& value : values) { value = 1; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 3, "range_for_loop_desugars_over_array: expected decl + loop + return");
    const scpp::Stmt& outer_block = *fn.body->statements[1];
    expect(outer_block.kind == scpp::StmtKind::Block,
           "range_for_loop_desugars_over_array: loop should desugar to a block");
    expect(outer_block.statements.size() == 3,
           "range_for_loop_desugars_over_array: expected hidden range + hidden index + while");
    if (outer_block.statements.size() != 3) return;
    expect(outer_block.statements[0]->kind == scpp::StmtKind::VarDecl &&
               outer_block.statements[0]->type.kind == scpp::TypeKind::Named &&
               outer_block.statements[0]->type.name == "auto" &&
               outer_block.statements[0]->var_name.rfind("$for_range_", 0) == 0,
           "range_for_loop_desugars_over_array: first hidden decl should store the range");
    expect(outer_block.statements[1]->kind == scpp::StmtKind::VarDecl &&
               outer_block.statements[1]->var_name.rfind("$for_index_", 0) == 0,
           "range_for_loop_desugars_over_array: second hidden decl should store the index");
    const scpp::Stmt& while_stmt = *outer_block.statements[2];
    expect(while_stmt.kind == scpp::StmtKind::While,
           "range_for_loop_desugars_over_array: third block stmt should be While");
    if (while_stmt.then_branch == nullptr || while_stmt.then_branch->statements.empty()) return;
    const scpp::Stmt& loop_var = *while_stmt.then_branch->statements[0];
    expect(loop_var.kind == scpp::StmtKind::VarDecl && loop_var.var_name == "value" &&
               loop_var.type.kind == scpp::TypeKind::Reference && loop_var.init != nullptr &&
               loop_var.init->kind == scpp::ExprKind::Subscript,
           "range_for_loop_desugars_over_array: first while-body stmt should bind loop variable from subscript");
}

void test_range_for_loop_desugars_over_span() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int main() { int values[3]; std::span<int> s = values; for (const auto& value : s) { x = value; } return 0; }");
    const scpp::Function& fn = program.functions.back();
    expect(fn.body->statements.size() == 4,
           "range_for_loop_desugars_over_span: expected array decl + span decl + loop + return");
    const scpp::Stmt& outer_block = *fn.body->statements[2];
    expect(outer_block.kind == scpp::StmtKind::Block,
           "range_for_loop_desugars_over_span: loop should desugar to a block");
    if (outer_block.statements.size() != 3) return;
    const scpp::Stmt& loop_var = *outer_block.statements[2]->then_branch->statements[0];
    expect(loop_var.kind == scpp::StmtKind::VarDecl && loop_var.var_name == "value" &&
               loop_var.type.kind == scpp::TypeKind::Reference && !loop_var.type.is_mutable_ref,
           "range_for_loop_desugars_over_span: loop variable should be const reference");
}

void test_range_for_loop_named_element_forms_desugar() {
    scpp::Program program = expect_parse_ok(
        "struct Box { int value = 0; };\n"
        "int main() {\n"
        "    Box boxes[2];\n"
        "    for (Box by_value : boxes) { return by_value.value; }\n"
        "    for (const Box& by_const_ref : boxes) { return by_const_ref.value; }\n"
        "    for (Box& by_ref : boxes) { return by_ref.value; }\n"
        "    return 0;\n"
        "}");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 5,
           "range_for_loop_named_element_forms_desugar: expected decl + 3 loops + return");
    if (fn.body->statements.size() != 5) return;

    const scpp::Stmt& by_value_loop = *fn.body->statements[1];
    const scpp::Stmt& by_const_ref_loop = *fn.body->statements[2];
    const scpp::Stmt& by_ref_loop = *fn.body->statements[3];
    expect(by_value_loop.kind == scpp::StmtKind::Block && by_value_loop.statements.size() == 3,
           "range_for_loop_named_element_forms_desugar: by-value loop should desugar to hidden decls plus while");
    expect(by_const_ref_loop.kind == scpp::StmtKind::Block && by_const_ref_loop.statements.size() == 3,
           "range_for_loop_named_element_forms_desugar: const-ref loop should desugar to hidden decls plus while");
    expect(by_ref_loop.kind == scpp::StmtKind::Block && by_ref_loop.statements.size() == 3,
           "range_for_loop_named_element_forms_desugar: ref loop should desugar to hidden decls plus while");
    if (by_value_loop.kind != scpp::StmtKind::Block || by_const_ref_loop.kind != scpp::StmtKind::Block ||
        by_ref_loop.kind != scpp::StmtKind::Block || by_value_loop.statements.size() != 3 ||
        by_const_ref_loop.statements.size() != 3 || by_ref_loop.statements.size() != 3 ||
        by_value_loop.statements[2]->then_branch == nullptr || by_const_ref_loop.statements[2]->then_branch == nullptr ||
        by_ref_loop.statements[2]->then_branch == nullptr || by_value_loop.statements[2]->then_branch->statements.empty() ||
        by_const_ref_loop.statements[2]->then_branch->statements.empty() ||
        by_ref_loop.statements[2]->then_branch->statements.empty()) {
        return;
    }
    const scpp::Stmt& by_value_var = *by_value_loop.statements[2]->then_branch->statements[0];
    const scpp::Stmt& by_const_ref_var = *by_const_ref_loop.statements[2]->then_branch->statements[0];
    const scpp::Stmt& by_ref_var = *by_ref_loop.statements[2]->then_branch->statements[0];

    expect(by_value_var.kind == scpp::StmtKind::VarDecl && by_value_var.var_name == "by_value" &&
               by_value_var.type.kind == scpp::TypeKind::Named && by_value_var.type.name == "Box",
           "range_for_loop_named_element_forms_desugar: by-value loop variable should keep the named element type");
    expect(by_const_ref_var.kind == scpp::StmtKind::VarDecl && by_const_ref_var.var_name == "by_const_ref" &&
               by_const_ref_var.type.kind == scpp::TypeKind::Reference && !by_const_ref_var.type.is_mutable_ref &&
               by_const_ref_var.type.pointee != nullptr && by_const_ref_var.type.pointee->kind == scpp::TypeKind::Named &&
               by_const_ref_var.type.pointee->name == "Box",
           "range_for_loop_named_element_forms_desugar: const-ref loop variable should keep const reference type");
    expect(by_ref_var.kind == scpp::StmtKind::VarDecl && by_ref_var.var_name == "by_ref" &&
               by_ref_var.type.kind == scpp::TypeKind::Reference && by_ref_var.type.is_mutable_ref &&
               by_ref_var.type.pointee != nullptr && by_ref_var.type.pointee->kind == scpp::TypeKind::Named &&
               by_ref_var.type.pointee->name == "Box",
           "range_for_loop_named_element_forms_desugar: ref loop variable should keep mutable reference type");
}

void test_range_for_body_parse_errors_are_not_misattributed_to_loop_var() {
    bool threw = false;
    if (auto _r = try_parse_with_std_imports(
            "import std;\n"
            "struct Capture { int value = 0; };\n"
            "struct Holder { std::vector<Capture> captures{}; };\n"
            "int sum(const Holder& holder) {\n"
            "    for (const Capture& capture : holder.captures) {\n"
            "        Capture scratch;\n"
            "        return capture.value + scratch.value;\n"
            "    }\n"
            "    return 0;\n"
            "}\n");
        !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(e.loc.line == 6,
               "range_for_body_parse_errors_are_not_misattributed_to_loop_var: expected body diagnostic on line 6");
        expect(std::string(e.what()).find("scratch{};") != std::string::npos,
               "range_for_body_parse_errors_are_not_misattributed_to_loop_var: diagnostic should name the body local");
        expect(std::string(e.what()).find("capture{};") == std::string::npos,
               "range_for_body_parse_errors_are_not_misattributed_to_loop_var: diagnostic should not be blamed on loop "
               "variable");
    }
    expect(threw, "range_for_body_parse_errors_are_not_misattributed_to_loop_var: expected a ParseError");
}

void test_unsafe_block_sets_is_unsafe_flag() {
    // `[[scpp::unsafe]] { }` (ch01 §1.3) is an ordinary Block statement
    // with is_unsafe set -- see parse_statement's attribute handling.
    scpp::Program program = expect_parse_ok("int f() { [[scpp::unsafe]] { int x = 1; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 2, "unsafe_block_sets_is_unsafe_flag: expected 2 statements");

    const scpp::Stmt& unsafe_block = *fn.body->statements[0];
    expect(unsafe_block.kind == scpp::StmtKind::Block,
           "unsafe_block_sets_is_unsafe_flag: should still be an ordinary Block");
    expect(unsafe_block.is_unsafe, "unsafe_block_sets_is_unsafe_flag: is_unsafe should be true");
    expect(unsafe_block.statements.size() == 1,
           "unsafe_block_sets_is_unsafe_flag: unsafe block should have 1 statement");
    expect(unsafe_block.statements[0]->kind == scpp::StmtKind::VarDecl,
           "unsafe_block_sets_is_unsafe_flag: nested statement should be VarDecl");
}

void test_ordinary_block_is_not_unsafe() {
    // Sanity check for the flag's default: a plain `{ }` (no
    // `[[scpp::unsafe]]` attribute) must never be mistaken for an
    // unsafe block.
    scpp::Program program = expect_parse_ok("int f() { { int x = 1; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& plain_block = *fn.body->statements[0];
    expect(plain_block.kind == scpp::StmtKind::Block, "ordinary_block_is_not_unsafe: should be a Block");
    expect(!plain_block.is_unsafe, "ordinary_block_is_not_unsafe: is_unsafe should be false");
}

void test_nested_unsafe_blocks_parse() {
    // `[[scpp::unsafe]] { [[scpp::unsafe]] { ... } }` (ch01 §1.3's
    // nesting rule) -- both levels independently set is_unsafe.
    scpp::Program program = expect_parse_ok("int f() { [[scpp::unsafe]] { [[scpp::unsafe]] { int x = 1; } } return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& outer = *fn.body->statements[0];
    expect(outer.is_unsafe, "nested_unsafe_blocks_parse: outer block should be unsafe");
    expect(outer.statements.size() == 1, "nested_unsafe_blocks_parse: outer block should have 1 statement");
    const scpp::Stmt& inner = *outer.statements[0];
    expect(inner.kind == scpp::StmtKind::Block, "nested_unsafe_blocks_parse: inner statement should be a Block");
    expect(inner.is_unsafe, "nested_unsafe_blocks_parse: inner block should also be unsafe");
}

// ch00 §2/ch01 §1.3: `unsafe` is no longer a keyword at all -- a bare
// `unsafe` (no `[[ ]]` brackets) is just an ordinary Identifier now, so
// `unsafe return 1;` fails to parse for a completely different reason
// than before (an Identifier expression-statement can't be followed
// directly by another statement-starting keyword like `return` with no
// operator/`;` in between) -- not because "unsafe" demands a `{` next.
void test_bare_unsafe_identifier_followed_by_return_is_parse_error() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { unsafe return 1; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "bare_unsafe_identifier_followed_by_return_is_parse_error: expected a ParseError to be thrown");
}

// ch01 §1.3: `[[scpp::unsafe]]` only has an effect when the statement it
// appertains to is a compound-statement (Block) -- on any other
// statement shape, it's parsed and silently ignored, exactly like a
// real C++ compiler accepts-and-ignores an attribute it doesn't act on
// in that position (mirrors `[[likely]] return 1;`, real, legal C++).
void test_unsafe_attribute_on_non_block_statement_has_no_effect() {
    scpp::Program program = expect_parse_ok("int f() { [[scpp::unsafe]] return 1; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.body->statements.size() == 1,
           "unsafe_attribute_on_non_block_statement_has_no_effect: expected 1 statement");
    expect(fn.body->statements[0]->kind == scpp::StmtKind::Return,
           "unsafe_attribute_on_non_block_statement_has_no_effect: should still parse as an ordinary Return");
}

// ch01 §1.2/§1.3: the function-level marker -- a leading
// `[[scpp::unsafe]]` before a function's own return type makes
// Function::is_unsafe true.
void test_function_level_unsafe_marker_parses() {
    scpp::Program program = expect_parse_ok("[[scpp::unsafe]] int f(int x) { return x; }\n"
                                         "int main() { return 0; }\n");
    const scpp::Function* f_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "f") f_fn = &fn;
    }
    expect(f_fn != nullptr, "function_level_unsafe_marker_parses: expected a Function named 'f'");
    expect(f_fn->is_unsafe, "function_level_unsafe_marker_parses: is_unsafe should be true");
}

void test_nodiscard_function_and_method_attributes_parse() {
    scpp::Program program = expect_parse_ok(
        "[[nodiscard(\"use the result\")]] int f() { return 1; }\n"
        "class Box {\n"
        "public:\n"
        "    [[nodiscard]] int value() const { return 7; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* f_fn = find_function_named(program, "f");
    const scpp::Function* value_fn = find_function_named(program, "Box_value");
    expect(f_fn != nullptr, "nodiscard_function_and_method_attributes_parse: expected a Function named 'f'");
    expect(f_fn->is_nodiscard, "nodiscard_function_and_method_attributes_parse: function should be nodiscard");
    expect(f_fn->nodiscard_reason == "use the result",
           "nodiscard_function_and_method_attributes_parse: function reason should parse");
    expect(value_fn != nullptr, "nodiscard_function_and_method_attributes_parse: expected method clone");
    expect(value_fn->is_nodiscard, "nodiscard_function_and_method_attributes_parse: method should be nodiscard");
    expect(value_fn->nodiscard_reason.empty(),
           "nodiscard_function_and_method_attributes_parse: bare nodiscard should have empty reason");
}

void test_inline_function_modifier_parses_with_existing_modifiers() {
    scpp::Program program = expect_parse_ok(
        "export module sample;\n"
        "export [[nodiscard(\"use it\")]] inline constexpr int answer() { return 42; }\n");
    const scpp::Function* fn = find_function_named(program, "answer");
    expect(fn != nullptr, "inline_function_modifier_parses_with_existing_modifiers: expected answer");
    if (fn == nullptr) return;
    expect(fn->is_exported, "inline_function_modifier_parses_with_existing_modifiers: function should be exported");
    expect(fn->is_nodiscard, "inline_function_modifier_parses_with_existing_modifiers: function should be nodiscard");
    expect(fn->nodiscard_reason == "use it",
           "inline_function_modifier_parses_with_existing_modifiers: nodiscard reason should parse");
    expect(fn->eval_mode == scpp::FunctionEvalMode::Constexpr,
           "inline_function_modifier_parses_with_existing_modifiers: constexpr should still parse");
}

void test_default_parameter_expression_parses() {
    scpp::Program program = expect_parse_ok(
        "int add(int lhs, int rhs = 1 + 2) { return lhs + rhs; }\n"
        "int zero(int value = {}) { return value; }\n"
        "class Box {\n"
        "public:\n"
        "    int value(int amount = 7) const { return amount; }\n"
        "};\n");
    const scpp::Function* add_fn = find_function_named(program, "add");
    const scpp::Function* zero_fn = find_function_named(program, "zero");
    const scpp::Function* value_fn = find_function_named(program, "Box_value");
    expect(add_fn != nullptr && zero_fn != nullptr, "default_parameter_expression_parses: expected free functions");
    expect(value_fn != nullptr, "default_parameter_expression_parses: expected Box_value");
    if (add_fn == nullptr || zero_fn == nullptr || value_fn == nullptr) return;
    expect(add_fn->params.size() == 2, "default_parameter_expression_parses: add should have 2 params");
    expect(add_fn->params[1].default_expr != nullptr,
           "default_parameter_expression_parses: rhs should have a default expression");
    expect(zero_fn->params[0].default_expr != nullptr,
           "default_parameter_expression_parses: empty-brace default should become an expression");
    expect(value_fn->params.size() == 2, "default_parameter_expression_parses: Box_value should include this + amount");
    expect(value_fn->params[1].default_expr != nullptr,
           "default_parameter_expression_parses: method parameter should retain default expression");
}

void test_default_parameter_trailing_rule_is_enforced() {
    bool threw = false;
    if (auto _r = scpp::parse("int bad(int x = 1, int y) { return x + y; }\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = std::string(e.what()).find("every later parameter must also have one") != std::string::npos;
    }
    expect(threw, "default_parameter_trailing_rule_is_enforced: expected trailing-only diagnostic");
}

void test_static_member_function_parses_without_this() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "public:\n"
        "    static int make(int value) { return value; }\n"
        "private:\n"
        "    static int secret() { return 7; }\n"
        "};\n"
        "int main() { return Box::make(3); }\n");
    const scpp::Function* make_fn = find_function_named(program, "Box_make");
    const scpp::Function* secret_fn = find_function_named(program, "Box_secret");
    expect(make_fn != nullptr, "static_member_function_parses_without_this: expected Box_make");
    expect(make_fn->is_static, "static_member_function_parses_without_this: method should be static");
    expect(make_fn->member_owner_class == "Box",
           "static_member_function_parses_without_this: owner class should be recorded");
    expect(make_fn->params.size() == 1 && make_fn->params[0].name == "value",
           "static_member_function_parses_without_this: static method should not get implicit this");
    expect(secret_fn != nullptr && secret_fn->access == scpp::AccessSpecifier::Private,
           "static_member_function_parses_without_this: private static access should parse");
}

void test_template_specialization_static_member_call_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Box;\n"
        "template<>\n"
        "class Box<int> {\n"
        "public:\n"
        "    static int make() { return 7; }\n"
        "};\n"
        "int main() { return Box<int>::make(); }\n");
    const scpp::Function* make_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.member_owner_class == "Box" && fn.is_static && fn.name.find("_make") != std::string::npos &&
            !fn.generic_method_owner_id.empty()) {
            make_fn = &fn;
            break;
        }
    }
    expect(make_fn != nullptr,
           "template_specialization_static_member_call_parses: expected specialized static make function");
}

void test_full_class_template_specialization_parses_as_concrete_specialization() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Box;\n"
        "template<>\n"
        "class Box<int> {\n"
        "public:\n"
        "    int value() const { return 2; }\n"
        "};\n"
        "int main() { Box<int> box{}; return box.value(); }\n");
    const scpp::ClassDef* specialization = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Box" && c.is_partial_specialization && c.template_params.empty()) {
            specialization = &c;
            break;
        }
    }
    expect(specialization != nullptr,
           "full_class_template_specialization_parses_as_concrete_specialization: expected explicit specialization");
    expect(specialization->specialization_template_args.size() == 1 &&
               specialization->specialization_template_args[0].kind == scpp::TypeKind::Named &&
               specialization->specialization_template_args[0].name == "int",
           "full_class_template_specialization_parses_as_concrete_specialization: expected concrete int argument");
}

void test_struct_forward_declaration_parses_and_reconciles() {
    scpp::Program program = expect_parse_ok(
        "struct Node;\n"
        "struct Node { Node* next; int value; };\n");
    expect(program.structs.size() == 2,
           "struct_forward_declaration_parses_and_reconciles: expected forward declaration plus definition");
    expect(program.structs[0].is_forward_declaration,
           "struct_forward_declaration_parses_and_reconciles: first declaration should be forward-only");
    expect(!program.structs[1].is_forward_declaration,
           "struct_forward_declaration_parses_and_reconciles: second declaration should be the full definition");
    expect(program.structs[1].fields.size() == 2,
           "struct_forward_declaration_parses_and_reconciles: full definition should keep its fields");
}

void test_class_forward_declaration_parses_and_reconciles() {
    scpp::Program program = expect_parse_ok(
        "class Box;\n"
        "class Box {\n"
        "public:\n"
        "    virtual ~Box() { return; }\n"
        "    Box* next{};\n"
        "};\n");
    expect(program.classes.size() == 2,
           "class_forward_declaration_parses_and_reconciles: expected forward declaration plus definition");
    expect(program.classes[0].is_forward_declaration,
           "class_forward_declaration_parses_and_reconciles: first declaration should be forward-only");
    expect(!program.classes[1].is_forward_declaration,
           "class_forward_declaration_parses_and_reconciles: second declaration should be the full definition");
}

void test_record_forward_declaration_tag_mismatch_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("struct Box;\nclass Box { public: virtual ~Box() { return; } };\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "record_forward_declaration_tag_mismatch_is_rejected: expected a ParseError");
}

// ch01 §1.3 (1): `[[scpp::unsafe]]` may only appertain to a compound-
// statement or a function's own declaration -- appertaining to a
// struct/class declaration is ill-formed.
void test_unsafe_attribute_on_struct_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("[[scpp::unsafe]] struct Foo { int x; };\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "unsafe_attribute_on_struct_is_rejected: expected a ParseError");
}

// ch05 §5.15: `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]` on a
// struct's own declaration set the manual-override flags.
void test_thread_safety_attribute_on_struct_parses() {
    scpp::Program program = expect_parse_ok(
        "struct [[scpp::thread_movable, nodiscard(\"keep this handle\")]] RawBufferHandle { int* data; int len; };\n"
        "int main() { return 0; }\n");
    const scpp::StructDef* s = nullptr;
    for (const scpp::StructDef& def : program.structs) {
        if (def.name == "RawBufferHandle") s = &def;
    }
    expect(s != nullptr, "thread_safety_attribute_on_struct_parses: expected a StructDef named 'RawBufferHandle'");
    expect(s->thread_movable_override, "thread_safety_attribute_on_struct_parses: thread_movable_override should be true");
    expect(!s->thread_shareable_override, "thread_safety_attribute_on_struct_parses: thread_shareable_override should be false");
    expect(s->is_nodiscard, "thread_safety_attribute_on_struct_parses: struct should be nodiscard");
    expect(s->nodiscard_reason == "keep this handle",
           "thread_safety_attribute_on_struct_parses: struct reason should parse");
}

// Same attribute grammar slot on a class's own declaration; both
// attributes may be given together, comma-separated inside one `[[...]]`.
void test_thread_safety_attributes_on_class_parse() {
    scpp::Program program = expect_parse_ok(
        "class [[scpp::thread_movable, scpp::thread_shareable]] Handle {\n"
        "public:\n"
        "    Handle(int* d) { this.data = d; return; }\n"
        "private:\n"
        "    int* data;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* c = nullptr;
    for (const scpp::ClassDef& def : program.classes) {
        if (def.name == "Handle") c = &def;
    }
    expect(c != nullptr, "thread_safety_attributes_on_class_parse: expected a ClassDef named 'Handle'");
    expect(c->thread_movable_override, "thread_safety_attributes_on_class_parse: thread_movable_override should be true");
    expect(c->thread_shareable_override, "thread_safety_attributes_on_class_parse: thread_shareable_override should be true");
}

// ch05 §5.15: attaching either attribute to a generic function's parameter
// (trailing, same slot as an ordinary declarator attribute) sets the
// constraint flag on that Param.
void test_thread_safety_attribute_on_parameter_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "void spawn(T&& f [[scpp::thread_movable]]) { return; }\n"
        "int main() { return 0; }\n");
    const scpp::Function* spawn_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "spawn") spawn_fn = &fn;
    }
    expect(spawn_fn != nullptr, "thread_safety_attribute_on_parameter_parses: expected a Function named 'spawn'");
    expect(spawn_fn->params.size() == 1, "thread_safety_attribute_on_parameter_parses: expected 1 param");
    expect(spawn_fn->params[0].require_thread_movable,
           "thread_safety_attribute_on_parameter_parses: require_thread_movable should be true");
    expect(!spawn_fn->params[0].require_thread_shareable,
           "thread_safety_attribute_on_parameter_parses: require_thread_shareable should be false");
}

// spec §6.4(1): a program shall not declare a move constructor for a
// class type -- exactly one parameter, of type rvalue reference to the
// class's own type. The compiler always provides one instead (spec
// §6.4(2)).
void test_user_declared_move_constructor_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("class Foo {\n"
                    "public:\n"
                    "    Foo() { return; }\n"
                    "    Foo(Foo&& other) { return; }\n"
                    "};\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "user_declared_move_constructor_is_rejected: expected a ParseError");
}

// An ordinary constructor taking a *different* type's rvalue reference
// (not the enclosing class's own) is not a move constructor at all, and
// must continue to parse normally.
void test_constructor_taking_other_type_rvalue_reference_parses() {
    scpp::Program program = expect_parse_ok(
        "class Foo {\n"
        "public:\n"
        "    Foo() { return; }\n"
        "};\n"
        "class Bar {\n"
        "public:\n"
        "    Bar(Foo&& f) { return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* bar_ctor = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Bar_new") bar_ctor = &fn;
    }
    expect(bar_ctor != nullptr, "constructor_taking_other_type_rvalue_reference_parses: expected a 'Bar_new' Function");
    expect(bar_ctor->params.size() == 2,
           "constructor_taking_other_type_rvalue_reference_parses: expected 2 params (this + f)");
}

// spec §6.5: `ReturnType operator=(Params) { ... }` parses as an
// ordinary method mangled to "ClassName_operator_assign", with the
// implicit `this` inserted as params[0] like any other method.
void test_operator_assign_parses() {
    scpp::Program program = expect_parse_ok(
        "class Widget {\n"
        "public:\n"
        "    Widget(int v) { this.v = v; return; }\n"
        "    Widget& operator=(const Widget& other) { this.v = other.v; return this; }\n"
        "    int v;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* op_assign = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Widget_operator_assign") op_assign = &fn;
    }
    expect(op_assign != nullptr, "operator_assign_parses: expected a 'Widget_operator_assign' Function");
    expect(op_assign->params.size() == 2, "operator_assign_parses: expected 2 params (this + other)");
    expect(op_assign->params[0].name == "this", "operator_assign_parses: params[0] should be 'this'");
    expect(op_assign->return_type.kind == scpp::TypeKind::Reference,
           "operator_assign_parses: return type should be a Reference ('Widget&')");
}

void test_out_of_line_constructor_definition_parses_and_merges() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "private:\n"
        "    int value_{};\n"
        "public:\n"
        "    virtual ~Box() = default;\n"
        "    Box(int value);\n"
        "    int value() const;\n"
        "};\n"
        "inline Box::Box(int value) : value_{value} { return; }\n"
        "int Box::value() const { return this->value_; }\n"
        "int main() { Box box{7}; return box.value() - 7; }\n");
    const scpp::Function* ctor = nullptr;
    const scpp::Function* value = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Box_new") ctor = &fn;
        if (fn.name == "Box_value") value = &fn;
    }
    expect(ctor != nullptr, "out_of_line_constructor_definition_parses_and_merges: expected Box_new");
    expect(value != nullptr, "out_of_line_constructor_definition_parses_and_merges: expected Box_value");
    if (ctor != nullptr) {
        expect(ctor->body != nullptr, "out_of_line_constructor_definition_parses_and_merges: ctor should have a body");
        expect(ctor->member_initializers.size() == 1,
               "out_of_line_constructor_definition_parses_and_merges: ctor should keep member initializer list");
    }
    if (value != nullptr) {
        expect(value->body != nullptr, "out_of_line_constructor_definition_parses_and_merges: method should have a body");
        expect(value->params.size() == 1 && value->params[0].type.kind == scpp::TypeKind::Reference &&
                   !value->params[0].type.is_mutable_ref,
               "out_of_line_constructor_definition_parses_and_merges: const method should synthesize const this");
    }
}

void test_out_of_line_destructor_definition_parses_and_merges() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "public:\n"
        "    virtual ~Box();\n"
        "};\n"
        "Box::~Box() { return; }\n"
        "int main() { Box box{}; return 0; }\n");
    const scpp::Function* dtor = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Box_delete") dtor = &fn;
    }
    expect(dtor != nullptr, "out_of_line_destructor_definition_parses_and_merges: expected Box_delete");
    if (dtor != nullptr) {
        expect(dtor->body != nullptr, "out_of_line_destructor_definition_parses_and_merges: destructor should have a body");
    }
}

void test_out_of_line_operator_assign_definition_parses_and_merges() {
    scpp::Program program = expect_parse_ok(
        "class Widget {\n"
        "public:\n"
        "    virtual ~Widget() = default;\n"
        "    Widget(int v) { this.v = v; return; }\n"
        "    Widget& operator=(const Widget& other);\n"
        "    int v{};\n"
        "};\n"
        "Widget& Widget::operator=(const Widget& other) { this.v = other.v; return this; }\n"
        "int main() { Widget w{1}; return w.v - 1; }\n");
    const scpp::Function* op_assign = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Widget_operator_assign") op_assign = &fn;
    }
    expect(op_assign != nullptr, "out_of_line_operator_assign_definition_parses_and_merges: expected operator=");
    if (op_assign != nullptr) {
        expect(op_assign->body != nullptr,
               "out_of_line_operator_assign_definition_parses_and_merges: operator= should have a body");
        expect(op_assign->params.size() == 2,
               "out_of_line_operator_assign_definition_parses_and_merges: operator= should keep this + other params");
    }
}

void test_out_of_line_member_definition_signature_mismatch_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "class Box {\n"
            "public:\n"
            "    virtual ~Box() = default;\n"
            "    int value() const;\n"
            "};\n"
            "bool Box::value() const { return true; }\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("does not match its earlier declaration exactly") != std::string::npos,
               "out_of_line_member_definition_signature_mismatch_is_rejected: expected mismatch diagnostic");
    }
    expect(threw, "out_of_line_member_definition_signature_mismatch_is_rejected: expected a ParseError");
}

// spec §6.4(1)/ch08 Q14: same unconditional rejection as
// test_user_declared_move_constructor_is_rejected, for
// `operator=(ClassName&&)` instead of the constructor -- a real,
// discovered-and-fixed gap: when `operator=` parsing was first added
// (test_operator_assign_parses above), the move-constructor shape check
// had no counterpart here at all, so a user-declared move assignment
// operator silently parsed as an ordinary (if unusual) overload instead
// of being rejected the same way the equivalent move constructor already
// correctly is.
void test_user_declared_move_assignment_operator_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("class Foo {\n"
                    "public:\n"
                    "    Foo() { return; }\n"
                    "    Foo& operator=(Foo&& other) { return this; }\n"
                    "};\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "user_declared_move_assignment_operator_is_rejected: expected a ParseError");
}

void test_defaulted_move_special_members_parse_without_parameter_names() {
    scpp::Program program = expect_parse_ok(
        "class Foo {\n"
        "public:\n"
        "    Foo() = default;\n"
        "    Foo(Foo&&) = default;\n"
        "    Foo& operator=(Foo&&) = default;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* move_ctor = nullptr;
    const scpp::Function* move_assign = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Foo_new" && fn.params.size() == 2 && fn.is_defaulted) move_ctor = &fn;
        if (fn.name == "Foo_operator_assign" && fn.params.size() == 2 && fn.is_defaulted) move_assign = &fn;
    }
    expect(move_ctor != nullptr && move_ctor->is_defaulted && move_ctor->params.size() == 2,
           "defaulted_move_special_members_parse_without_parameter_names: expected defaulted move constructor");
    expect(move_assign != nullptr && move_assign->is_defaulted && move_assign->params.size() == 2,
           "defaulted_move_special_members_parse_without_parameter_names: expected defaulted move assignment");
}

void test_defaulted_non_special_member_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("class Foo {\n"
                    "public:\n"
                    "    int value() = default;\n"
                    "};\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "defaulted_non_special_member_is_rejected: expected a ParseError");
}

void test_equality_operator_methods_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Point {\n"
        "    int x = 0;\n"
        "    int y = 0;\n"
        "    bool operator==(const Point&) const = default;\n"
        "    bool operator!=(const Point& other) const { return this.x != other.x; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* eq = find_function_named(program, "Point_operator_equal");
    const scpp::Function* ne = find_function_named(program, "Point_operator_not_equal");
    expect(eq != nullptr && eq->is_defaulted && eq->params.size() == 2,
           "equality_operator_methods_parse: expected defaulted operator==");
    expect(ne != nullptr && !ne->is_defaulted && ne->params.size() == 2,
           "equality_operator_methods_parse: expected user-defined operator!=");
}

// ch06 §6: `static_cast<T>(expr)` parses to a Cast expression with `type`
// set to the target type and the operand in `lhs`.
void test_static_cast_parses() {
    scpp::Program program = expect_parse_ok("int f(bool b) { return static_cast<int>(b); }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return, "static_cast_parses: expected Return");
    const scpp::Expr& cast = *ret.expr;
    expect(cast.kind == scpp::ExprKind::Cast, "static_cast_parses: expected Cast");
    expect(is_named_type(cast.type, "int"), "static_cast_parses: target type should be 'int'");
    expect(cast.lhs != nullptr && cast.lhs->kind == scpp::ExprKind::Identifier && cast.lhs->name == "b",
           "static_cast_parses: operand should be Identifier 'b'");
}

// ch06 §6: `(T)expr` (the C-style cast) parses identically to
// `static_cast<T>(expr)` -- same Cast node shape.
void test_c_style_cast_parses() {
    scpp::Program program = expect_parse_ok("int f(bool b) { return (int)b; }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    const scpp::Expr& cast = *ret.expr;
    expect(cast.kind == scpp::ExprKind::Cast, "c_style_cast_parses: expected Cast");
    expect(is_named_type(cast.type, "int"), "c_style_cast_parses: target type should be 'int'");
    expect(cast.lhs != nullptr && cast.lhs->kind == scpp::ExprKind::Identifier && cast.lhs->name == "b",
           "c_style_cast_parses: operand should be Identifier 'b'");
}

// A parenthesized expression that merely *starts* with an identifier
// (never a registered type name) must still parse as ordinary grouping,
// not be misdetected as a C-style cast -- e.g. `(x)` where `x` is a
// plain local variable, or `(x + y)`.
void test_parenthesized_expression_is_not_misdetected_as_cast() {
    scpp::Program program = expect_parse_ok("int f(int x, int y) { return (x) + (x + y) * 2; }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    expect(ret.expr->kind == scpp::ExprKind::Binary,
           "parenthesized_expression_is_not_misdetected_as_cast: expected Binary");
}

void test_sizeof_type_expression_parses() {
    scpp::Program program = expect_parse_ok("int f() { return (int)sizeof(int); }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    const scpp::Expr& cast = *ret.expr;
    expect(cast.kind == scpp::ExprKind::Cast, "sizeof_type_expression_parses: expected outer Cast");
    expect(cast.lhs != nullptr && cast.lhs->kind == scpp::ExprKind::Sizeof,
           "sizeof_type_expression_parses: cast operand should be Sizeof");
    expect(cast.lhs->sizeof_operand_is_type, "sizeof_type_expression_parses: expected sizeof(type) form");
    expect(is_named_type(cast.lhs->type, "int"), "sizeof_type_expression_parses: queried type should be 'int'");
}

void test_sizeof_value_expression_parses() {
    scpp::Program program = expect_parse_ok("int f() { return (int)sizeof(x + 1); }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    const scpp::Expr& cast = *ret.expr;
    expect(cast.kind == scpp::ExprKind::Cast, "sizeof_value_expression_parses: expected outer Cast");
    expect(cast.lhs != nullptr && cast.lhs->kind == scpp::ExprKind::Sizeof,
           "sizeof_value_expression_parses: cast operand should be Sizeof");
    expect(!cast.lhs->sizeof_operand_is_type, "sizeof_value_expression_parses: expected sizeof(expr) form");
    expect(cast.lhs->lhs != nullptr && cast.lhs->lhs->kind == scpp::ExprKind::Binary &&
               cast.lhs->lhs->binary_op == scpp::BinaryOp::Add,
           "sizeof_value_expression_parses: operand should be Binary Add");
}

void test_operator_precedence() {
    // 1 + 2 * 3 should parse as 1 + (2 * 3), not (1 + 2) * 3.
    scpp::Program program = expect_parse_ok("int f() { return 1 + 2 * 3; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0].get()->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Add,
           "operator_precedence: top-level op should be Add");
    expect(expr.lhs->kind == scpp::ExprKind::IntegerLiteral && expr.lhs->int_value == 1,
           "operator_precedence: lhs should be IntegerLiteral 1");
    expect(expr.rhs->kind == scpp::ExprKind::Binary && expr.rhs->binary_op == scpp::BinaryOp::Mul,
           "operator_precedence: rhs should be Mul");
}

void test_unary_and_call() {
    scpp::Program program = expect_parse_ok("int f() { return -foo(1, 2); }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::Neg,
           "unary_and_call: top-level should be unary Neg");
    expect(expr.lhs->kind == scpp::ExprKind::Call && expr.lhs->name == "foo",
           "unary_and_call: operand should be call to 'foo'");
    expect(expr.lhs->args.size() == 2, "unary_and_call: expected 2 args");
    expect(expr.lhs->args[0]->int_value == 1 && expr.lhs->args[1]->int_value == 2,
           "unary_and_call: args should be 1 and 2");
}

void test_dereference_expression() {
    scpp::Program program = expect_parse_ok("int f() { return *p; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::Deref,
           "dereference_expression: top-level should be unary Deref");
    expect(expr.lhs->kind == scpp::ExprKind::Identifier && expr.lhs->name == "p",
           "dereference_expression: operand should be identifier 'p'");
}

void test_address_of_plain_variable() {
    // `&x` (ch05 §5.7) -- a new prefix unary operator, sibling to Deref.
    scpp::Program program = expect_parse_ok("int f() { return &x; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_plain_variable: top-level should be unary AddressOf");
    expect(expr.lhs->kind == scpp::ExprKind::Identifier && expr.lhs->name == "x",
           "address_of_plain_variable: operand should be identifier 'x'");
}

void test_address_of_field_and_subscript() {
    // `&p.x` and `&arr[i]` -- same operand shapes already accepted as a
    // borrow source for `T&`/`const T&` (ch05.2), reused here.
    scpp::Program field_program = expect_parse_ok("int f() { return &p.x; }");
    const scpp::Expr& field_expr = *field_program.functions[0].body->statements[0]->expr;
    expect(field_expr.kind == scpp::ExprKind::Unary && field_expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_field_and_subscript: &p.x top-level should be unary AddressOf");
    expect(field_expr.lhs->kind == scpp::ExprKind::Member && field_expr.lhs->name == "x",
           "address_of_field_and_subscript: &p.x operand should be Member 'x'");

    scpp::Program subscript_program = expect_parse_ok("int f() { return &arr[i]; }");
    const scpp::Expr& subscript_expr = *subscript_program.functions[0].body->statements[0]->expr;
    expect(subscript_expr.kind == scpp::ExprKind::Unary && subscript_expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_field_and_subscript: &arr[i] top-level should be unary AddressOf");
    expect(subscript_expr.lhs->kind == scpp::ExprKind::Subscript,
           "address_of_field_and_subscript: &arr[i] operand should be Subscript");
}

void test_address_of_dereference_chain() {
    // `&*p` -- address-of applied to a dereference, recursing off Deref
    // just like Neg/Not/Deref's own operands already do (parse_unary).
    scpp::Program program = expect_parse_ok("int f() { return &*p; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_dereference_chain: top-level should be unary AddressOf");
    expect(expr.lhs->kind == scpp::ExprKind::Unary && expr.lhs->unary_op == scpp::UnaryOp::Deref,
           "address_of_dereference_chain: operand should be unary Deref");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Identifier && expr.lhs->lhs->name == "p",
           "address_of_dereference_chain: innermost operand should be identifier 'p'");
}

void test_increment_and_decrement_operators_parse() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    int x = 5;\n"
        "    int a = ++x;\n"
        "    int b = x++;\n"
        "    int c = --x;\n"
        "    int d = x--;\n"
        "    for (int i = 0; i < 3; i++) { }\n"
        "    return a + b + c + d;\n"
        "}\n");
    const scpp::Function& main_fn = program.functions[0];
    expect(main_fn.body->statements.size() == 7,
           "increment_and_decrement_operators_parse: expected 7 top-level statements");
    auto expect_unary_init = [&](std::size_t stmt_index, scpp::UnaryOp op, std::string_view label) {
        if (stmt_index >= main_fn.body->statements.size()) return;
        const scpp::Stmt& stmt = *main_fn.body->statements[stmt_index];
        expect(stmt.kind == scpp::StmtKind::VarDecl && stmt.init != nullptr,
               std::string(label) + ": expected initialized VarDecl");
        if (stmt.kind != scpp::StmtKind::VarDecl || stmt.init == nullptr) return;
        expect(stmt.init->kind == scpp::ExprKind::Unary && stmt.init->unary_op == op,
               std::string(label) + ": expected unary increment/decrement initializer");
        expect(stmt.init->lhs != nullptr && stmt.init->lhs->kind == scpp::ExprKind::Identifier && stmt.init->lhs->name == "x",
               std::string(label) + ": expected operand identifier 'x'");
    };
    expect_unary_init(1, scpp::UnaryOp::PreInc, "increment_and_decrement_operators_parse pre-inc");
    expect_unary_init(2, scpp::UnaryOp::PostInc, "increment_and_decrement_operators_parse post-inc");
    expect_unary_init(3, scpp::UnaryOp::PreDec, "increment_and_decrement_operators_parse pre-dec");
    expect_unary_init(4, scpp::UnaryOp::PostDec, "increment_and_decrement_operators_parse post-dec");
    const scpp::Stmt& loop_block = *main_fn.body->statements[5];
    expect(loop_block.kind == scpp::StmtKind::Block,
           "increment_and_decrement_operators_parse: for-loop should desugar to a block");
    if (loop_block.kind != scpp::StmtKind::Block || loop_block.statements.size() != 2) return;
    const scpp::Stmt& while_stmt = *loop_block.statements[1];
    expect(while_stmt.kind == scpp::StmtKind::While && while_stmt.then_branch != nullptr &&
               while_stmt.then_branch->kind == scpp::StmtKind::Block,
           "increment_and_decrement_operators_parse: expected while body block");
    if (while_stmt.kind != scpp::StmtKind::While || while_stmt.then_branch == nullptr ||
        while_stmt.then_branch->statements.size() != 2) {
        return;
    }
    const scpp::Stmt& increment_stmt = *while_stmt.then_branch->statements[1];
    expect(increment_stmt.kind == scpp::StmtKind::ExprStmt && increment_stmt.expr != nullptr &&
               increment_stmt.expr->kind == scpp::ExprKind::Unary &&
               increment_stmt.expr->unary_op == scpp::UnaryOp::PostInc,
           "increment_and_decrement_operators_parse: expected postfix ++ in desugared increment clause");
}

void test_compound_assignment_operators_parse() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    int x = 1;\n"
        "    x += 2;\n"
        "    x -= 3;\n"
        "    x *= 4;\n"
        "    x /= 5;\n"
        "    int y = 1;\n"
        "    x += y += 6;\n"
        "    return x;\n"
        "}\n");
    const scpp::Function& main_fn = program.functions[0];
    expect(main_fn.body->statements.size() == 8,
           "compound_assignment_operators_parse: expected 8 top-level statements");
    auto expect_compound_stmt = [&](std::size_t stmt_index, scpp::BinaryOp op, std::string_view label) {
        if (stmt_index >= main_fn.body->statements.size()) return;
        const scpp::Stmt& stmt = *main_fn.body->statements[stmt_index];
        expect(stmt.kind == scpp::StmtKind::ExprStmt && stmt.expr != nullptr && stmt.expr->kind == scpp::ExprKind::Binary &&
                   stmt.expr->binary_op == op,
               std::string(label) + ": expected binary compound assignment");
    };
    expect_compound_stmt(1, scpp::BinaryOp::AddAssign, "compound_assignment_operators_parse +=");
    expect_compound_stmt(2, scpp::BinaryOp::SubAssign, "compound_assignment_operators_parse -=");
    expect_compound_stmt(3, scpp::BinaryOp::MulAssign, "compound_assignment_operators_parse *=");
    expect_compound_stmt(4, scpp::BinaryOp::DivAssign, "compound_assignment_operators_parse /=");
    const scpp::Stmt& chained = *main_fn.body->statements[6];
    expect(chained.kind == scpp::StmtKind::ExprStmt && chained.expr != nullptr && chained.expr->kind == scpp::ExprKind::Binary &&
               chained.expr->binary_op == scpp::BinaryOp::AddAssign,
           "compound_assignment_operators_parse: outer chained expr should be +=");
    if (chained.kind != scpp::StmtKind::ExprStmt || chained.expr == nullptr || chained.expr->kind != scpp::ExprKind::Binary) return;
    expect(chained.expr->rhs != nullptr && chained.expr->rhs->kind == scpp::ExprKind::Binary &&
               chained.expr->rhs->binary_op == scpp::BinaryOp::AddAssign,
           "compound_assignment_operators_parse: rhs should parse as right-associative +=");
}

void test_local_type_definitions_parse() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    struct Point {\n"
        "        int x;\n"
        "        int y;\n"
        "        int sum() const { return this->x + this->y; }\n"
        "    };\n"
        "    Point p{};\n"
        "    p.x = 2;\n"
        "    p.y = 3;\n"
        "    return p.sum();\n"
        "}\n");
    expect(program.structs.size() == 1, "local_type_definitions_parse: expected one local struct definition");
    expect(program.functions.size() == 2, "local_type_definitions_parse: expected main plus one local method");
    if (program.structs.empty() || program.functions.size() < 2) return;
    const scpp::StructDef& point = program.structs[0];
    expect(point.name != "Point" && point.name.ends_with("::Point"),
           "local_type_definitions_parse: local struct should use an internal qualified name");
    expect(point.fields.size() == 2, "local_type_definitions_parse: expected two fields");
    const scpp::Function& main_fn = program.functions.back();
    expect(main_fn.body != nullptr && main_fn.body->statements.size() == 5,
           "local_type_definitions_parse: expected local-type placeholder plus four executable statements");
    if (!main_fn.body || main_fn.body->statements.size() < 2) return;
    expect(main_fn.body->statements[0]->kind == scpp::StmtKind::Block && main_fn.body->statements[0]->statements.empty(),
           "local_type_definitions_parse: local type definition should lower to an empty placeholder block statement");
    const scpp::Stmt& decl = *main_fn.body->statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl && decl.type.kind == scpp::TypeKind::Named && decl.type.name == point.name,
           "local_type_definitions_parse: later local variable should resolve to the local struct type");
}

void test_arrow_parses_as_deferred_operator_arrow_access() {
    scpp::Program program = expect_parse_ok("int f() { return p->x; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Member && expr.name == "x",
           "arrow_parses_as_deferred_operator_arrow_access: top-level should be Member 'x'");
    expect(expr.through_arrow,
           "arrow_parses_as_deferred_operator_arrow_access: member should remember it came from '->'");
    expect(expr.lhs->kind == scpp::ExprKind::Identifier && expr.lhs->name == "p",
           "arrow_parses_as_deferred_operator_arrow_access: receiver should stay as identifier 'p'");
}

void test_chained_arrow_and_dot() {
    scpp::Program program = expect_parse_ok("int f() { return p->x.y; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Member && expr.name == "y",
           "chained_arrow_and_dot: outer should be Member 'y'");
    expect(expr.lhs->kind == scpp::ExprKind::Member && expr.lhs->name == "x",
           "chained_arrow_and_dot: middle should be Member 'x'");
    expect(expr.lhs->through_arrow,
           "chained_arrow_and_dot: inner member should remember '->'");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Identifier && expr.lhs->lhs->name == "p",
           "chained_arrow_and_dot: inner receiver should remain identifier 'p'");
}

void test_operator_arrow_member_decl_and_explicit_call_parse() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "public:\n"
        "    int* operator->() [[scpp::lifetime(this)]];\n"
        "};\n"
        "int* f(Box& b) { return b.operator->(); }\n");
    const scpp::Function* op = find_function_named(program, "Box_operator_arrow");
    expect(op != nullptr, "operator_arrow_member_decl_and_explicit_call_parse: missing synthesized operator-> function");
    expect(op->params.size() == 1 && op->params[0].name == "this",
           "operator_arrow_member_decl_and_explicit_call_parse: operator-> should only have implicit this");
    expect(op->return_lifetime.present() && op->return_lifetime.name == "this",
           "operator_arrow_member_decl_and_explicit_call_parse: return lifetime should parse");
    const scpp::Expr& expr = *program.functions.back().body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Call && expr.name == "operator_arrow",
           "operator_arrow_member_decl_and_explicit_call_parse: explicit call should parse as method call");
    expect(!expr.through_arrow,
           "operator_arrow_member_decl_and_explicit_call_parse: explicit .operator->() must not use arrow protocol");
}

void test_member_function_lifetime_this_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Box {\n"
        "    int value;\n"
        "    int* ptr() [[scpp::lifetime(this)]];\n"
        "};\n");
    const scpp::Function* fn = find_function_named(program, "Box_ptr");
    expect(fn != nullptr, "member_function_lifetime_this_parse: missing synthesized member function");
    expect(fn->return_lifetime.name == "this",
           "member_function_lifetime_this_parse: expected return lifetime 'this'");
    expect(fn->params.size() == 1 && fn->params[0].name == "this",
           "member_function_lifetime_this_parse: expected only implicit this param");
}

void test_member_decl_return_lifetime_this_parse() {
    scpp::Program program = expect_parse_ok(
        "class Box {\n"
        "public:\n"
        "    int* data() [[scpp::lifetime(this)]];\n"
        "};\n");
    const scpp::Function* fn = find_function_named(program, "Box_data");
    expect(fn != nullptr, "member_decl_return_lifetime_this_parse: missing synthesized member function");
    expect(fn != nullptr && fn->params.size() == 1 && fn->params[0].name == "this",
           "member_decl_return_lifetime_this_parse: member should only have implicit this");
    expect(fn != nullptr && fn->return_lifetime.present() && fn->return_lifetime.name == "this",
           "member_decl_return_lifetime_this_parse: return lifetime should parse as 'this'");
}


void test_return_brace_constructed_optional_reference_wrapper_parses_as_call() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "class Foo { public: virtual ~Foo() = default; Foo() = default; Foo(const Foo&) = default; };\n"
        "using OptionalFooRef = std::optional<std::reference_wrapper<const Foo>>;\n"
        "OptionalFooRef f(const Foo& x [[scpp::lifetime(source)]]) [[scpp::lifetime(source)]] {\n"
        "    return OptionalFooRef{std::reference_wrapper<const Foo>{x}};\n"
        "}\n");
    const scpp::Function* fn = find_function_named(program, "f");
    expect(fn != nullptr, "return_brace_constructed_optional_reference_wrapper_parses_as_call: expected function 'f'");
    const scpp::Expr& expr = *fn->body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Call && expr.name == "OptionalFooRef",
           "return_brace_constructed_optional_reference_wrapper_parses_as_call: outer should parse as call to alias");
    expect(expr.args.size() == 1 && expr.args[0]->kind == scpp::ExprKind::Call,
           "return_brace_constructed_optional_reference_wrapper_parses_as_call: expected nested call argument");
    expect(expr.args[0]->name == "std::reference_wrapper",
           "return_brace_constructed_optional_reference_wrapper_parses_as_call: inner should parse as reference_wrapper ctor");
    expect(expr.args[0]->args.size() == 1 && expr.args[0]->args[0]->kind == scpp::ExprKind::Identifier &&
               expr.args[0]->args[0]->name == "x",
           "return_brace_constructed_optional_reference_wrapper_parses_as_call: inner ctor should keep identifier arg");
}

void test_nested_reference_wrapper_lifetime_parameter_parse() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int* lookup(std::optional<std::reference_wrapper<const int [[scpp::lifetime(source)]]>> source) "
        "[[scpp::lifetime(source)]] { return nullptr; }\n");
    const scpp::Function* fn = find_function_named(program, "lookup");
    expect(fn != nullptr, "nested_reference_wrapper_lifetime_parameter_parse: expected function 'lookup'");
    expect(fn != nullptr && fn->params.size() == 1 && fn->params[0].lifetime.present() &&
               fn->params[0].lifetime.name == "source",
           "nested_reference_wrapper_lifetime_parameter_parse: expected nested parameter lifetime to hoist");
    expect(fn != nullptr && fn->return_lifetime.present() && fn->return_lifetime.name == "source",
           "nested_reference_wrapper_lifetime_parameter_parse: expected return lifetime to parse");
}

void test_multiplication_is_not_confused_with_dereference() {
    // `a * b` (binary multiply) must stay distinct from a leading `*b`
    // (unary deref) -- see parse_unary's comment.
    scpp::Program program = expect_parse_ok("int f() { return a * b; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Mul,
           "multiplication_is_not_confused_with_dereference: should be Binary Mul, not Unary Deref");
}

void test_parenthesized_expression() {
    // (1 + 2) * 3 should parse with the addition grouped first.
    scpp::Program program = expect_parse_ok("int f() { return (1 + 2) * 3; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Mul,
           "parenthesized_expression: top-level op should be Mul");
    expect(expr.lhs->kind == scpp::ExprKind::Binary && expr.lhs->binary_op == scpp::BinaryOp::Add,
           "parenthesized_expression: lhs should be Add");
}

void test_parse_error_on_missing_semicolon() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { return 1 }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "parse_error_on_missing_semicolon: expected a ParseError to be thrown");
}

void test_struct_declaration() {
    scpp::Program program = expect_parse_ok("struct Point { int x; int y; }; int f() { return 0; }");
    expect(program.structs.size() == 1, "struct_declaration: expected 1 struct");
    const scpp::StructDef& def = program.structs[0];
    expect(def.name == "Point", "struct_declaration: name should be 'Point'");
    expect(!def.is_union, "struct_declaration: ordinary struct should not be marked as a union");
    expect(!def.is_packed, "struct_declaration: ordinary struct should not be marked as packed");
    expect(def.fields.size() == 2, "struct_declaration: expected 2 fields");
    expect(is_named_type(def.fields[0].type, "int") && def.fields[0].name == "x",
           "struct_declaration: field 0 should be 'int x'");
    expect(is_named_type(def.fields[1].type, "int") && def.fields[1].name == "y",
           "struct_declaration: field 1 should be 'int y'");
    expect(program.functions.size() == 1, "struct_declaration: expected 1 function after the struct");
}

void test_struct_access_specifier_sections_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Box {\n"
        "public:\n"
        "    int value{};\n"
        "private:\n"
        "    int hidden{};\n"
        "};\n"
        "int main() {\n"
        "    Box box{};\n"
        "    return box.value;\n"
        "}\n");
    expect(program.structs.size() == 1, "struct_access_specifier_sections_parse: expected 1 struct");
    if (program.structs.size() != 1) return;
    const scpp::StructDef& def = program.structs[0];
    expect(def.fields.size() == 2, "struct_access_specifier_sections_parse: expected 2 fields");
    if (def.fields.size() != 2) return;
    expect(def.fields[0].name == "value" && def.fields[0].access == scpp::AccessSpecifier::Public,
           "struct_access_specifier_sections_parse: value should stay public");
    expect(def.fields[1].name == "hidden" && def.fields[1].access == scpp::AccessSpecifier::Private,
           "struct_access_specifier_sections_parse: hidden should stay private");
}

void test_struct_constructors_and_methods_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Size {\n"
        "public:\n"
        "    int width{};\n"
        "    Size() {}\n"
        "    Size(int side) : width{side}, height{side} {}\n"
        "private:\n"
        "    int height{};\n"
        "    int hidden() const { return height; }\n"
        "public:\n"
        "    Size(int w, int h) : width{w}, height{h} {}\n"
        "    int area() const { return width * height; }\n"
        "    int perimeter() { return (width + hidden()) * 2; }\n"
        "};\n");
    const scpp::StructDef* size = find_struct_named(program, "Size");
    expect(size != nullptr, "struct_constructors_and_methods_parse: expected a StructDef named 'Size'");
    if (size == nullptr) return;
    expect(size->fields.size() == 2, "struct_constructors_and_methods_parse: expected 2 fields");
    if (size->fields.size() != 2) return;
    expect(size->fields[0].name == "width" && size->fields[0].access == scpp::AccessSpecifier::Public,
           "struct_constructors_and_methods_parse: width should stay public");
    expect(size->fields[1].name == "height" && size->fields[1].access == scpp::AccessSpecifier::Private,
           "struct_constructors_and_methods_parse: height should stay private");

    int ctor_count = 0;
    const scpp::Function* converting_ctor = nullptr;
    const scpp::Function* area = nullptr;
    const scpp::Function* perimeter = nullptr;
    const scpp::Function* hidden = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.member_owner_class != "Size") continue;
        if (fn.name == "Size_new") {
            ctor_count++;
            if (fn.params.size() == 2) converting_ctor = &fn;
            continue;
        }
        if (fn.name == "Size_area") area = &fn;
        if (fn.name == "Size_perimeter") perimeter = &fn;
        if (fn.name == "Size_hidden") hidden = &fn;
    }
    expect(ctor_count == 3, "struct_constructors_and_methods_parse: expected 3 constructors");
    expect(converting_ctor != nullptr, "struct_constructors_and_methods_parse: expected single-parameter converting ctor");
    if (converting_ctor != nullptr) {
        expect(converting_ctor->member_initializers.size() == 2,
               "struct_constructors_and_methods_parse: converting ctor should preserve member initializer list");
    }
    expect(area != nullptr, "struct_constructors_and_methods_parse: expected const method 'area'");
    if (area != nullptr) {
        expect(area->params.size() == 1 && area->params[0].name == "this" &&
                   area->params[0].type.kind == scpp::TypeKind::Reference && !area->params[0].type.is_mutable_ref &&
                   area->params[0].type.pointee != nullptr && is_named_type(*area->params[0].type.pointee, "Size"),
               "struct_constructors_and_methods_parse: const method should receive const-like this reference");
    }
    expect(perimeter != nullptr && perimeter->access == scpp::AccessSpecifier::Public,
           "struct_constructors_and_methods_parse: expected public method 'perimeter'");
    expect(hidden != nullptr && hidden->access == scpp::AccessSpecifier::Private,
           "struct_constructors_and_methods_parse: expected private method 'hidden'");
}

void test_struct_interface_attribute_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("struct [[scpp::interface]] Box { int value{}; };"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("shall not be marked '[[scpp::interface]]'") != std::string::npos,
               "struct_interface_attribute_is_rejected: expected interface-only diagnostic");
    }
    expect(threw, "struct_interface_attribute_is_rejected: expected a ParseError");
}

void test_struct_base_clause_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "class Base {\n"
            "public:\n"
            "    Base() { return; }\n"
            "};\n"
            "struct Derived : public Base {\n"
            "    int value{};\n"
            "};\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("shall not have a base-clause") != std::string::npos,
               "struct_base_clause_is_rejected: expected base-clause diagnostic");
    }
    expect(threw, "struct_base_clause_is_rejected: expected a ParseError");
}

void test_struct_virtual_member_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "struct Box {\n"
            "public:\n"
            "    virtual int value();\n"
            "};\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("shall not declare a virtual member function") != std::string::npos,
               "struct_virtual_member_is_rejected: expected virtual-member diagnostic");
    }
    expect(threw, "struct_virtual_member_is_rejected: expected a ParseError");
}

void test_union_declaration() {
    scpp::Program program = expect_parse_ok("union Payload { int i; char c; }; int f() { return 0; }");
    expect(program.structs.size() == 1, "union_declaration: expected 1 aggregate");
    const scpp::StructDef& def = program.structs[0];
    expect(def.name == "Payload", "union_declaration: name should be 'Payload'");
    expect(def.is_union, "union_declaration: should be marked as a union");
    expect(!def.is_packed, "union_declaration: plain union should not be marked as packed");
    expect(def.fields.size() == 2, "union_declaration: expected 2 members");
    expect(is_named_type(def.fields[0].type, "int") && def.fields[0].name == "i",
           "union_declaration: member 0 should be 'int i'");
    expect(is_named_type(def.fields[1].type, "char") && def.fields[1].name == "c",
           "union_declaration: member 1 should be 'char c'");
}

void test_packed_struct_and_union_attributes_parse() {
    scpp::Program program = expect_parse_ok(
        "struct [[scpp::packed]] Event { char tag; int value; };"
        "union [[scpp::packed]] Bits { int i; char raw[4]; };"
        "int f() { return 0; }");
    expect(program.structs.size() == 2, "packed_struct_and_union_attributes_parse: expected 2 aggregates");
    expect(program.structs[0].name == "Event", "packed_struct_and_union_attributes_parse: first aggregate");
    expect(program.structs[0].is_packed, "packed_struct_and_union_attributes_parse: struct should be packed");
    expect(!program.structs[0].is_union, "packed_struct_and_union_attributes_parse: Event should be a struct");
    expect(program.structs[1].name == "Bits", "packed_struct_and_union_attributes_parse: second aggregate");
    expect(program.structs[1].is_packed, "packed_struct_and_union_attributes_parse: union should be packed");
    expect(program.structs[1].is_union, "packed_struct_and_union_attributes_parse: Bits should be a union");
}

void test_packed_attribute_on_function_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("[[scpp::packed]]\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        const scpp::ParseError& e = _r.error();
        threw = true;
        expect(std::string(e.what()).find("only to a struct or union declaration") != std::string::npos,
               "packed_attribute_on_function_is_rejected: diagnostic should mention struct/union-only support");
    }
    expect(threw, "packed_attribute_on_function_is_rejected: expected a ParseError");
}

void test_struct_variable_and_member_access() {
    scpp::Program program = expect_parse_ok(
        "struct Point { int x; int y; };"
        "int f() {"
        "    Point p{};"
        "    p.x = 1;"
        "    return p.x + p.y;"
        "}");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "struct_variable_and_member_access: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "Point" && decl.var_name == "p",
           "struct_variable_and_member_access: decl should be 'Point p'");
    expect(decl.has_ctor_args && decl.ctor_args.empty(),
           "struct_variable_and_member_access: Point p{} should parse as empty brace-init");

    const scpp::Stmt& assign_stmt = *fn.body->statements[1];
    expect(assign_stmt.kind == scpp::StmtKind::ExprStmt,
           "struct_variable_and_member_access: statement 1 should be ExprStmt");
    const scpp::Expr& assign = *assign_stmt.expr;
    expect(assign.kind == scpp::ExprKind::Binary && assign.binary_op == scpp::BinaryOp::Assign,
           "struct_variable_and_member_access: expr should be an Assign");
    expect(assign.lhs->kind == scpp::ExprKind::Member && assign.lhs->name == "x",
           "struct_variable_and_member_access: assign target should be Member 'x'");
    expect(assign.lhs->lhs->kind == scpp::ExprKind::Identifier && assign.lhs->lhs->name == "p",
           "struct_variable_and_member_access: member base should be identifier 'p'");

    const scpp::Stmt& ret = *fn.body->statements[2];
    expect(ret.expr->kind == scpp::ExprKind::Binary && ret.expr->binary_op == scpp::BinaryOp::Add,
           "struct_variable_and_member_access: return expr should be Add");
    expect(ret.expr->lhs->kind == scpp::ExprKind::Member && ret.expr->lhs->name == "x",
           "struct_variable_and_member_access: lhs should be Member 'x'");
    expect(ret.expr->rhs->kind == scpp::ExprKind::Member && ret.expr->rhs->name == "y",
           "struct_variable_and_member_access: rhs should be Member 'y'");
}

void test_nested_member_access() {
    scpp::Program program = expect_parse_ok(
        "struct Inner { int v; };"
        "struct Outer { Inner inner; };"
        "int f() {"
        "    Outer o{};"
        "    return o.inner.v;"
        "}");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& ret = *fn.body->statements[1];
    const scpp::Expr& expr = *ret.expr;
    expect(expr.kind == scpp::ExprKind::Member && expr.name == "v", "nested_member_access: outer should be Member 'v'");
    expect(expr.lhs->kind == scpp::ExprKind::Member && expr.lhs->name == "inner",
           "nested_member_access: inner should be Member 'inner'");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Identifier && expr.lhs->lhs->name == "o",
           "nested_member_access: base should be identifier 'o'");
}

void test_pointer_field_type() {
    scpp::Program program = expect_parse_ok("struct Node { int value; Node* next; };");
    const scpp::StructDef& def = program.structs[0];
    expect(def.fields[1].name == "next", "pointer_field_type: field 1 should be named 'next'");
    const scpp::Type& next_type = def.fields[1].type;
    expect(next_type.kind == scpp::TypeKind::Pointer, "pointer_field_type: field 1 should be a Pointer type");
    expect(next_type.pointee != nullptr && is_named_type(*next_type.pointee, "Node"),
           "pointer_field_type: pointee should be named 'Node'");
}

void test_array_field_and_subscript() {
    scpp::Program program = expect_parse_ok(
        "struct Buffer { int values[4]; };"
        "int f() {"
        "    Buffer b{};"
        "    b.values[0] = 1;"
        "    return b.values[0];"
        "}");
    const scpp::StructDef& def = program.structs[0];
    const scpp::Type& values_type = def.fields[0].type;
    expect(values_type.kind == scpp::TypeKind::Array, "array_field_and_subscript: field should be an Array type");
    // ch05 §9.4: the parser only captures the bound as a deferred
    // constant-expression (`array_size_expr`); it is resolved into
    // `array_size` later, by the constant-expression evaluation pipeline
    // -- not by the parser itself.
    expect(values_type.array_size_expr != nullptr &&
               values_type.array_size_expr->kind == scpp::ExprKind::IntegerLiteral &&
               values_type.array_size_expr->int_value == 4,
           "array_field_and_subscript: array_size_expr should be IntegerLiteral 4");
    expect(values_type.element != nullptr && is_named_type(*values_type.element, "int"),
           "array_field_and_subscript: element type should be 'int'");

    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& assign_stmt = *fn.body->statements[1];
    const scpp::Expr& assign = *assign_stmt.expr;
    expect(assign.lhs->kind == scpp::ExprKind::Subscript,
           "array_field_and_subscript: assign target should be Subscript");
    expect(assign.lhs->lhs->kind == scpp::ExprKind::Member && assign.lhs->lhs->name == "values",
           "array_field_and_subscript: subscript base should be Member 'values'");
    expect(assign.lhs->rhs->kind == scpp::ExprKind::IntegerLiteral && assign.lhs->rhs->int_value == 0,
           "array_field_and_subscript: subscript index should be 0");
}

void test_array_parameter_decays_to_pointer() {
    // ch02 §2.1: a fixed-size array parameter decays to a pointer to its
    // element type, exactly as in ordinary C++ (`int arr[4]` and
    // `int* arr` are the same parameter type) -- needed so `extern "C"`
    // signatures can use arrays "in parameter position", per the spec.
    scpp::Program program = expect_parse_ok("int f(int arr[4]) { return arr[0]; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 1, "array_parameter_decays_to_pointer: expected 1 parameter");
    const scpp::Type& param_type = fn.params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer,
           "array_parameter_decays_to_pointer: parameter type should be Pointer, not Array");
    expect(param_type.pointee != nullptr && is_named_type(*param_type.pointee, "int"),
           "array_parameter_decays_to_pointer: pointee should be 'int'");
}

void test_local_array_declaration() {
    scpp::Program program = expect_parse_ok("int f() { int values[8]; return values[0]; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "local_array_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Array && decl.type.array_size_expr != nullptr &&
               decl.type.array_size_expr->kind == scpp::ExprKind::IntegerLiteral &&
               decl.type.array_size_expr->int_value == 8,
           "local_array_declaration: type should be an Array with array_size_expr IntegerLiteral 8");
    expect(decl.type.element != nullptr && is_named_type(*decl.type.element, "int"),
           "local_array_declaration: element type should be 'int'");
}

void test_struct_before_use_is_required() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { Point p{}; return 0; } struct Point { int x; };"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "struct_before_use_is_required: expected a ParseError when Point is used before declaration");
}

void test_unique_ptr_type_declaration() {
    scpp::Program program = parse_with_std_imports("import std;\nint f() { std::unique_ptr<int> a{}; return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "unique_ptr_type_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "std::unique_ptr",
           "unique_ptr_type_declaration: type should be std::unique_ptr");
    expect(decl.type.template_args.size() == 1 && is_named_type(decl.type.template_args[0], "int"),
           "unique_ptr_type_declaration: pointee should be 'int'");
    expect(decl.var_name == "a", "unique_ptr_type_declaration: variable name should be 'a'");
    expect(decl.has_ctor_args && decl.ctor_args.empty(),
           "unique_ptr_type_declaration: unique_ptr local should use empty brace-init");
}

void test_unique_ptr_of_struct_type() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "struct Point { int x; int y; };"
        "int f() { std::unique_ptr<Point> a{}; return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "std::unique_ptr",
           "unique_ptr_of_struct_type: type should be std::unique_ptr");
    expect(decl.type.template_args.size() == 1 && is_named_type(decl.type.template_args[0], "Point"),
           "unique_ptr_of_struct_type: pointee should be 'Point'");
}

void test_span_type_declaration() {
    scpp::Program program = expect_parse_ok("int f() { int arr[3]; std::span<int> s = arr; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "span_type_declaration: statement 1 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Span, "span_type_declaration: type should be Span");
    expect(decl.type.pointee != nullptr && is_named_type(*decl.type.pointee, "int"),
           "span_type_declaration: element type should be 'int'");
    expect(decl.type.is_mutable_ref, "span_type_declaration: std::span<int> should be mutable (is_mutable_ref)");
    expect(decl.var_name == "s", "span_type_declaration: variable name should be 's'");
}

void test_std_string_view_type_and_calls_parse() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "bool same(std::string_view view, const std::string& text) {\n"
        "    return view == std::string_view{text};\n"
        "}\n"
        "int main() {\n"
        "    std::string text{\"owner::Type\"};\n"
        "    std::string_view view{text};\n"
        "    size_t pos = view.rfind(\"::\");\n"
        "    return same(view.substr(pos + 2), text) ? 0 : 1;\n"
        "}\n");
    const scpp::Function* same = find_function_named(program, "same");
    expect(same != nullptr, "std_string_view_type_and_calls_parse: expected same()");
    if (same != nullptr) {
        expect(same->params.size() == 2, "std_string_view_type_and_calls_parse: expected 2 params");
        expect(is_named_type(same->params[0].type, "std::string_view"),
               "std_string_view_type_and_calls_parse: first param should be std::string_view");
    }
}

void test_span_of_const_element_type() {
    scpp::Program program = expect_parse_ok("int f() { int arr[3]; std::span<const int> s = arr; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.type.kind == scpp::TypeKind::Span, "span_of_const_element_type: type should be Span");
    expect(!decl.type.is_mutable_ref,
           "span_of_const_element_type: std::span<const int> should be read-only (!is_mutable_ref)");
}

void test_move_expression() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int f() {"
        "    std::unique_ptr<int> a{};"
        "    std::unique_ptr<int> b = std::move(a);"
        "    return 0;"
        "}");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "move_expression: statement 1 should be VarDecl");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::Move,
           "move_expression: initializer should be a Move expression");
    expect(decl.init->lhs->kind == scpp::ExprKind::Identifier && decl.init->lhs->name == "a",
           "move_expression: moved expression should be identifier 'a'");
}

void test_move_as_function_argument() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int consume(std::unique_ptr<int> p) { return 0; }"
        "int f() {"
        "    std::unique_ptr<int> a{};"
        "    return consume(std::move(a));"
        "}");
    const scpp::Function& consume_fn = *find_function_named(program, "consume");
    expect(consume_fn.params.size() == 1 && consume_fn.params[0].type.kind == scpp::TypeKind::Named &&
               consume_fn.params[0].type.name == "std::unique_ptr",
           "move_as_function_argument: 'consume' should take a std::unique_ptr param");

    const scpp::Function& f_fn = *find_function_named(program, "f");
    const scpp::Stmt& ret = *f_fn.body->statements[1];
    expect(ret.expr->kind == scpp::ExprKind::Call && ret.expr->name == "consume",
           "move_as_function_argument: return expr should be a call to 'consume'");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::Move,
           "move_as_function_argument: call argument should be a Move expression");
}

void test_brace_init_return_expression() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "std::string make_text() {"
        "    return std::string{\"hello\"};"
        "}");
    const scpp::Function& fn = *find_function_named(program, "make_text");
    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return && ret.expr != nullptr,
           "brace_init_return_expression: expected Return with an expression");
    expect(ret.expr->kind == scpp::ExprKind::Call && ret.expr->name == "std::string",
           "brace_init_return_expression: return expr should parse as a class-construction Call");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::StringLiteral &&
               ret.expr->args[0]->name == "hello",
           "brace_init_return_expression: expected one string literal constructor argument");
}

void test_generic_type_brace_init_return_expression_parses() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "std::optional<int> make_value() {\n"
        "    return std::optional<int>{7};\n"
        "}\n");
    const scpp::Function& fn = *find_function_named(program, "make_value");
    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return && ret.expr != nullptr,
           "generic_type_brace_init_return_expression_parses: expected Return with expression");
    expect(ret.expr->kind == scpp::ExprKind::Call && ret.expr->name == "std::optional",
           "generic_type_brace_init_return_expression_parses: expected a Call to std::optional");
    expect(ret.expr->explicit_template_args.size() == 1 && ret.expr->explicit_template_args[0].is_type &&
               is_named_type(ret.expr->explicit_template_args[0].type, "int"),
           "generic_type_brace_init_return_expression_parses: expected explicit_template_args == [int]");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::IntegerLiteral &&
               ret.expr->args[0]->int_value == 7,
           "generic_type_brace_init_return_expression_parses: expected integer constructor argument");
}

void test_generic_type_declaration_brace_init_still_parses_as_ctor_args() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int main() {\n"
        "    std::optional<int> value{7};\n"
        "    return value.has_value() ? 0 : 1;\n"
        "}\n");
    const scpp::Function& fn = *find_function_named(program, "main");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl,
           "generic_type_declaration_brace_init_still_parses_as_ctor_args: first statement should be VarDecl");
    expect(is_named_type(decl.type, "std::optional") && decl.type.template_args.size() == 1 &&
               is_named_type(decl.type.template_args[0], "int"),
           "generic_type_declaration_brace_init_still_parses_as_ctor_args: decl type should stay std::optional<int>");
    expect(decl.has_ctor_args && decl.ctor_args.size() == 1,
           "generic_type_declaration_brace_init_still_parses_as_ctor_args: expected one ctor arg");
    expect(decl.init == nullptr,
           "generic_type_declaration_brace_init_still_parses_as_ctor_args: brace init should not become an expression initializer");
}

void test_generic_type_ctad_brace_init_return_expression_parses() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "std::optional<int> make_value() {\n"
        "    return std::optional{7};\n"
        "}\n");
    const scpp::Function& fn = *find_function_named(program, "make_value");
    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.expr != nullptr && ret.expr->kind == scpp::ExprKind::Call && ret.expr->name == "std::optional",
           "generic_type_ctad_brace_init_return_expression_parses: expected a Call to std::optional");
    expect(ret.expr->explicit_template_args.empty(),
           "generic_type_ctad_brace_init_return_expression_parses: CTAD form should have no explicit_template_args");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::IntegerLiteral &&
               ret.expr->args[0]->int_value == 7,
           "generic_type_ctad_brace_init_return_expression_parses: expected integer constructor argument");
}

void test_make_unique_zero_args() {
    scpp::Program program =
        parse_with_std_imports("import std;\nint f() { std::unique_ptr<int> a = std::make_unique<int>(); return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::Call && decl.init->name == "std::make_unique",
           "make_unique_zero_args: initializer should be a std::make_unique call");
    expect(decl.init->explicit_template_args.size() == 1 && decl.init->explicit_template_args[0].is_type &&
               is_named_type(decl.init->explicit_template_args[0].type, "int"),
           "make_unique_zero_args: element type should be 'int'");
    expect(decl.init->args.empty(), "make_unique_zero_args: expected 0 arguments");
}

void test_make_unique_with_arg() {
    scpp::Program program = parse_with_std_imports(
        "import std;\nint f() { std::unique_ptr<int> a = std::make_unique<int>(42); return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::Call && decl.init->name == "std::make_unique",
           "make_unique_with_arg: initializer should be a std::make_unique call");
    expect(decl.init->args.size() == 1 && decl.init->args[0]->kind == scpp::ExprKind::IntegerLiteral &&
               decl.init->args[0]->int_value == 42,
           "make_unique_with_arg: expected a single IntegerLiteral 42 argument");
}

void test_make_unique_of_struct_type() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "struct Point { int x; int y; };"
        "int f() { std::unique_ptr<Point> a = std::make_unique<Point>(); return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::Call && decl.init->name == "std::make_unique",
           "make_unique_of_struct_type: initializer should be a std::make_unique call");
    expect(decl.init->explicit_template_args.size() == 1 && decl.init->explicit_template_args[0].is_type &&
               is_named_type(decl.init->explicit_template_args[0].type, "Point"),
           "make_unique_of_struct_type: element type should be 'Point'");
}

void test_new_and_delete_parse() {
    scpp::Program program = expect_parse_ok(
        "int f() { [[scpp::unsafe]] { int* p = new int(7); delete p; } return 0; }");
    const scpp::Stmt& unsafe_block = *program.functions[0].body->statements[0];
    expect(unsafe_block.kind == scpp::StmtKind::Block && unsafe_block.is_unsafe,
           "new_and_delete_parse: first statement should be an unsafe block");
    expect(unsafe_block.statements.size() == 2, "new_and_delete_parse: unsafe block should have 2 statements");
    const scpp::Stmt& decl = *unsafe_block.statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "new_and_delete_parse: statement 0 should be VarDecl");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::New,
           "new_and_delete_parse: initializer should be a New expression");
    expect(is_named_type(decl.init->type, "int"), "new_and_delete_parse: new element type should be 'int'");
    expect(decl.init->has_paren_init, "new_and_delete_parse: new int(7) should record paren-init");
    expect(decl.init->args.size() == 1 && decl.init->args[0]->kind == scpp::ExprKind::IntegerLiteral &&
               decl.init->args[0]->int_value == 7,
           "new_and_delete_parse: expected a single IntegerLiteral ctor arg");
    const scpp::Stmt& del = *unsafe_block.statements[1];
    expect(del.kind == scpp::StmtKind::ExprStmt, "new_and_delete_parse: statement 1 should be ExprStmt");
    expect(del.expr != nullptr && del.expr->kind == scpp::ExprKind::Delete,
           "new_and_delete_parse: expr should be a Delete expression");
    expect(del.expr->lhs != nullptr && del.expr->lhs->kind == scpp::ExprKind::Identifier && del.expr->lhs->name == "p",
           "new_and_delete_parse: delete operand should be identifier 'p'");
}

void test_placement_new_parse() {
    scpp::Program program = expect_parse_ok(
        "int f() { [[scpp::unsafe]] { alignas(int) char slot[sizeof(int)]{}; int* p = new ((int*)&slot) int(7); } return 0; }");
    const scpp::Stmt& unsafe_block = *program.functions[0].body->statements[0];
    const scpp::Stmt& decl = *unsafe_block.statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "placement_new_parse: statement 1 should be VarDecl");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::New,
           "placement_new_parse: initializer should be a New expression");
    expect(decl.init->lhs != nullptr && decl.init->lhs->kind == scpp::ExprKind::Cast,
           "placement_new_parse: placement operand should parse as a Cast");
    expect(decl.init->has_paren_init, "placement_new_parse: placement new should preserve ctor paren-init");
}

void test_explicit_destructor_parse() {
    scpp::Program program = expect_parse_ok(
        "class Box { public: ~Box() { return; } }; int f() { [[scpp::unsafe]] { Box* p = (Box*)0; p->~Box(); } return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& unsafe_block = *fn.body->statements[0];
    const scpp::Stmt& stmt = *unsafe_block.statements[1];
    expect(stmt.kind == scpp::StmtKind::ExprStmt, "explicit_destructor_parse: statement 1 should be ExprStmt");
    expect(stmt.expr != nullptr && stmt.expr->kind == scpp::ExprKind::Destroy,
           "explicit_destructor_parse: expr should be a Destroy expression");
    expect(stmt.expr->destroy_through_pointer, "explicit_destructor_parse: expected pointer-form destructor call");
    expect(is_named_type(stmt.expr->type, "Box"), "explicit_destructor_parse: destroyed type should be Box");
}

void test_full_header_parameter_pack_and_new_pack_expansion_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename T, typename... Args>\n"
        "T* make_it(Args... args) { [[scpp::unsafe]] { return new T(args...); } }\n");
    expect(program.functions.size() == 1,
           "full_header_parameter_pack_and_new_pack_expansion_parse: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.template_params.size() == 2,
           "full_header_parameter_pack_and_new_pack_expansion_parse: expected 2 template params");
    expect(fn.template_params[1].is_pack,
           "full_header_parameter_pack_and_new_pack_expansion_parse: Args should be a pack");
    expect(fn.params.size() == 1 && fn.params[0].is_parameter_pack,
           "full_header_parameter_pack_and_new_pack_expansion_parse: parameter should be a pack");
    expect(fn.params[0].name == "args",
           "full_header_parameter_pack_and_new_pack_expansion_parse: parameter name should be 'args'");
    expect(fn.body->statements.size() == 1,
           "full_header_parameter_pack_and_new_pack_expansion_parse: expected 1 top-level statement");
    const scpp::Stmt& unsafe_block = *fn.body->statements[0];
    expect(unsafe_block.kind == scpp::StmtKind::Block && unsafe_block.is_unsafe,
           "full_header_parameter_pack_and_new_pack_expansion_parse: statement should be an unsafe block");
    expect(unsafe_block.statements.size() == 1 && unsafe_block.statements[0]->kind == scpp::StmtKind::Return,
           "full_header_parameter_pack_and_new_pack_expansion_parse: unsafe block should contain a Return");
    const scpp::Stmt& ret = *unsafe_block.statements[0];
    expect(ret.expr != nullptr && ret.expr->kind == scpp::ExprKind::New,
           "full_header_parameter_pack_and_new_pack_expansion_parse: return expr should be New");
    expect(is_named_type(ret.expr->type, "T"),
           "full_header_parameter_pack_and_new_pack_expansion_parse: new element type should be 'T'");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::PackExpansion,
           "full_header_parameter_pack_and_new_pack_expansion_parse: ctor arg should be PackExpansion");
    expect(ret.expr->args[0]->lhs != nullptr && ret.expr->args[0]->lhs->kind == scpp::ExprKind::Identifier &&
               ret.expr->args[0]->lhs->name == "args",
           "full_header_parameter_pack_and_new_pack_expansion_parse: pack expansion should expand 'args'");
}

void test_full_header_transformed_pointer_parameter_pack_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename... Tail>\n"
        "int bridge(Tail*... ptrs) { return 0; }\n");
    expect(program.functions.size() == 1,
           "full_header_transformed_pointer_parameter_pack_parses: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.template_params.size() == 1 && fn.template_params[0].is_pack,
           "full_header_transformed_pointer_parameter_pack_parses: expected Tail pack template parameter");
    expect(fn.params.size() == 1 && fn.params[0].is_parameter_pack,
           "full_header_transformed_pointer_parameter_pack_parses: expected pointer parameter pack");
    expect(fn.params[0].type.kind == scpp::TypeKind::Pointer && fn.params[0].type.pointee != nullptr &&
               is_named_type(*fn.params[0].type.pointee, "Tail"),
           "full_header_transformed_pointer_parameter_pack_parses: expected Tail* parameter type");
}

void test_full_header_wrapped_template_parameter_pack_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Token {\n"
        "public:\n"
        "    Token() { return; }\n"
        "};\n"
        "template<typename... Tail>\n"
        "int bridge(Token<Tail>... tokens) { return 0; }\n");
    expect(program.functions.size() == 2,
           "full_header_wrapped_template_parameter_pack_parses: expected Token ctor plus bridge");
    const scpp::Function& fn = program.functions.back();
    expect(fn.template_params.size() == 1 && fn.template_params[0].is_pack,
           "full_header_wrapped_template_parameter_pack_parses: expected Tail pack template parameter");
    expect(fn.params.size() == 1 && fn.params[0].is_parameter_pack,
           "full_header_wrapped_template_parameter_pack_parses: expected wrapped parameter pack");
    expect(fn.params[0].type.kind == scpp::TypeKind::Named && fn.params[0].type.name == "Token" &&
               fn.params[0].type.template_args.size() == 1 && is_named_type(fn.params[0].type.template_args[0], "Tail"),
           "full_header_wrapped_template_parameter_pack_parses: expected Token<Tail> parameter type");
}

void test_extern_c_single_declaration() {
    // ch02 §2.1: a bodyless `extern "C"` declaration.
    scpp::Program program = expect_parse_ok("extern \"C\" int c_abs(int n); int main() { return 0; }");
    expect(program.functions.size() == 2, "extern_c_single_declaration: expected 2 functions");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_extern_c, "extern_c_single_declaration: is_extern_c should be true");
    expect(fn.body == nullptr, "extern_c_single_declaration: body should be null (no definition)");
    expect(fn.name == "c_abs", "extern_c_single_declaration: name should be 'c_abs'");
    expect(fn.params.size() == 1 && is_named_type(fn.params[0].type, "int"),
           "extern_c_single_declaration: expected 1 int parameter");
}

void test_extern_c_block_form() {
    // ch02 §2.1: the block form is sugar for repeating `extern "C"` on
    // each nested declaration.
    scpp::Program program = expect_parse_ok(
        "extern \"C\" {"
        "    int c_abs(int n);"
        "    void c_exit(int code);"
        "}"
        "int main() { return 0; }");
    expect(program.functions.size() == 3, "extern_c_block_form: expected 3 functions");
    expect(program.functions[0].is_extern_c && program.functions[0].body == nullptr,
           "extern_c_block_form: 'c_abs' should be an extern declaration");
    expect(program.functions[1].is_extern_c && program.functions[1].body == nullptr,
           "extern_c_block_form: 'c_exit' should be an extern declaration");
    expect(is_named_type(program.functions[1].return_type, "void"),
           "extern_c_block_form: 'c_exit' should return 'void'");
}

void test_extern_c_definition_is_checked_like_any_function() {
    // ch02 §2.1: an `extern "C"` *definition* (a body present) is an
    // ordinary, fully-checked function that additionally requests C
    // linkage -- every function is checked by default (ch01), so there's
    // no separate flag to assert here beyond is_extern_c/body itself.
    scpp::Program program = expect_parse_ok("extern \"C\" int add(int a, int b) { return a + b; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_extern_c, "extern_c_definition_is_checked_like_any_function: is_extern_c should be true");
    expect(fn.body != nullptr, "extern_c_definition_is_checked_like_any_function: body should be present");
}

void test_extern_cpp_linkage_is_rejected() {
    // v0.1 only accepts the literal "C" linkage string.
    bool threw = false;
    if (auto _r = scpp::parse("extern \"C++\" int foo(int x);"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "extern_cpp_linkage_is_rejected: expected a ParseError");
}

void test_extern_c_varargs_declaration() {
    // ch02 §2.1: `...` is parsed and stored as has_varargs on a bodyless
    // extern "C" declaration.
    scpp::Program program = expect_parse_ok("extern \"C\" int my_printf(int fmt, ...);");
    const scpp::Function& fn = program.functions[0];
    expect(fn.has_varargs, "extern_c_varargs_declaration: has_varargs should be true");
    expect(fn.params.size() == 1, "extern_c_varargs_declaration: expected exactly 1 named parameter");
}

void test_extern_c_function_pointer_parameter_declaration() {
    scpp::Program program = expect_parse_ok(
        "extern \"C\" void* scpp_thread_spawn(void (*trampoline)(void*), void* arg);"
        "int main() { return 0; }");
    expect(program.functions.size() == 2,
           "extern_c_function_pointer_parameter_declaration: expected 2 functions");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_extern_c, "extern_c_function_pointer_parameter_declaration: should be extern C");
    expect(fn.params.size() == 2,
           "extern_c_function_pointer_parameter_declaration: expected 2 parameters");
    expect(fn.params[0].name == "trampoline",
           "extern_c_function_pointer_parameter_declaration: first param name should be trampoline");
    expect(fn.params[0].type.kind == scpp::TypeKind::FunctionPointer,
           "extern_c_function_pointer_parameter_declaration: first param should be a function pointer");
    expect(fn.params[0].type.function_return != nullptr && is_named_type(*fn.params[0].type.function_return, "void"),
           "extern_c_function_pointer_parameter_declaration: trampoline should return void");
    expect(fn.params[0].type.function_params.size() == 1 &&
               fn.params[0].type.function_params[0].kind == scpp::TypeKind::Pointer &&
               fn.params[0].type.function_params[0].pointee != nullptr &&
               is_named_type(*fn.params[0].type.function_params[0].pointee, "void"),
           "extern_c_function_pointer_parameter_declaration: trampoline should take a void*");
    expect(fn.params[1].type.kind == scpp::TypeKind::Pointer && fn.params[1].type.pointee != nullptr &&
               is_named_type(*fn.params[1].type.pointee, "void"),
           "extern_c_function_pointer_parameter_declaration: second param should be void*");
}

void test_varargs_on_definition_is_rejected() {
    // v0.1 only supports `...` on a bodyless extern "C" declaration, not
    // a definition (ch02 §2.1).
    bool threw = false;
    if (auto _r = scpp::parse("extern \"C\" int f(int a, ...) { return a; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "varargs_on_definition_is_rejected: expected a ParseError");
}

void test_varargs_on_non_extern_function_is_rejected() {
    // `...` is only meaningful for extern "C" declarations (ch02 §2.1).
    bool threw = false;
    if (auto _r = scpp::parse("int f(int a, ...);"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "varargs_on_non_extern_function_is_rejected: expected a ParseError");
}

void test_void_return_and_void_pointer_types() {
    // ch02 §2.1's `void` prerequisite: valid as a return type and as a
    // pointer's pointee.
    scpp::Program program = expect_parse_ok("extern \"C\" void free(void* p);");
    const scpp::Function& fn = program.functions[0];
    expect(is_named_type(fn.return_type, "void"), "void_return_and_void_pointer_types: return type should be 'void'");
    expect(fn.params.size() == 1 && fn.params[0].type.kind == scpp::TypeKind::Pointer,
           "void_return_and_void_pointer_types: parameter should be a pointer");
    expect(is_named_type(*fn.params[0].type.pointee, "void"),
           "void_return_and_void_pointer_types: parameter's pointee should be 'void'");
}

void test_char_type_declaration() {
    scpp::Program program = expect_parse_ok("int f() { char c{}; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "char_type_declaration: statement should be VarDecl");
    expect(is_named_type(decl.type, "char"), "char_type_declaration: type should be 'char'");
}

void test_char_literal_expression() {
    scpp::Program program = expect_parse_ok("int f() { char c = 'a'; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::CharLiteral,
           "char_literal_expression: initializer should be a CharLiteral");
    expect(decl.init->int_value == 'a', "char_literal_expression: ordinal value should be 'a' (97)");
}

void test_char_literal_escape_sequences_decode_correctly() {
    struct Case { const char* source; long long expected; };
    const Case cases[] = {
        {"int f() { char c = '\\n'; return 0; }", '\n'},
        {"int f() { char c = '\\t'; return 0; }", '\t'},
        {"int f() { char c = '\\r'; return 0; }", '\r'},
        {"int f() { char c = '\\\\'; return 0; }", '\\'},
        {"int f() { char c = '\\''; return 0; }", '\''},
        {"int f() { char c = '\\0'; return 0; }", '\0'},
    };
    for (const Case& c : cases) {
        scpp::Program program = expect_parse_ok(c.source);
        const scpp::Stmt& decl = *program.functions[0].body->statements[0];
        expect(decl.init->int_value == c.expected,
               "char_literal_escape_sequences_decode_correctly: mismatch for " + std::string(c.source));
    }
}

void test_empty_char_literal_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { char c = ''; return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "empty_char_literal_is_rejected: expected a ParseError");
}

void test_multi_character_char_literal_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { char c = 'ab'; return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "multi_character_char_literal_is_rejected: expected a ParseError");
}

void test_unsupported_char_escape_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { char c = '\\z'; return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "unsupported_char_escape_is_rejected: expected a ParseError");
}

void test_string_literal_expression() {
    scpp::Program program = expect_parse_ok("int f(char* p) { p = \"hello\"; return 0; }");
    const scpp::Expr& assign = *program.functions[0].body->statements[0]->expr;
    expect(assign.rhs->kind == scpp::ExprKind::StringLiteral,
           "string_literal_expression: rhs should be a StringLiteral");
    expect(assign.rhs->name == "hello", "string_literal_expression: decoded content should be 'hello'");
}

void test_string_literal_escape_sequences_decode_correctly() {
    struct Case { const char* source; const char* expected; };
    const Case cases[] = {
        {"int f(char* p) { p = \"a\\nb\"; return 0; }", "a\nb"},
        {"int f(char* p) { p = \"\\t\\r\"; return 0; }", "\t\r"},
        {"int f(char* p) { p = \"a\\\\b\"; return 0; }", "a\\b"},
        {"int f(char* p) { p = \"say \\\"hi\\\"\"; return 0; }", "say \"hi\""},
    };
    for (const Case& c : cases) {
        scpp::Program program = expect_parse_ok(c.source);
        const scpp::Expr& assign = *program.functions[0].body->statements[0]->expr;
        expect(assign.rhs->name == c.expected,
               "string_literal_escape_sequences_decode_correctly: mismatch for " + std::string(c.source));
    }
}

void test_empty_string_literal_is_allowed() {
    // Unlike an empty char literal (always rejected -- there's no ordinal
    // value for it to hold), an empty string is a perfectly ordinary,
    // zero-length C string.
    scpp::Program program = expect_parse_ok("int f(char* p) { p = \"\"; return 0; }");
    const scpp::Expr& assign = *program.functions[0].body->statements[0]->expr;
    expect(assign.rhs->kind == scpp::ExprKind::StringLiteral,
           "empty_string_literal_is_allowed: rhs should be a StringLiteral");
    expect(assign.rhs->name.empty(), "empty_string_literal_is_allowed: decoded content should be empty");
}

void test_unsupported_string_escape_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f(char* p) { p = \"\\z\"; return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "unsupported_string_escape_is_rejected: expected a ParseError");
}

void test_const_char_pointer_type() {
    // `const T*` (ch02 §2.1's realistic C signature compatibility -- e.g.
    // `const char* fmt`) parses as its own distinct Pointer type: scpp
    // now properly tracks pointer constness via is_mutable_pointee (ch05
    // §5.7, ch08 Q9), rather than silently dropping `const`.
    scpp::Program program = expect_parse_ok("extern \"C\" int puts(const char* s);");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 1, "const_char_pointer_type: expected 1 parameter");
    const scpp::Type& param_type = fn.params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer, "const_char_pointer_type: parameter should be Pointer");
    expect(param_type.pointee != nullptr && is_named_type(*param_type.pointee, "char"),
           "const_char_pointer_type: pointee should be 'char'");
    expect(!param_type.is_mutable_pointee, "const_char_pointer_type: is_mutable_pointee should be false");
}

void test_plain_pointer_defaults_to_mutable_pointee() {
    // `T*` (no `const`) should default to is_mutable_pointee == true --
    // the common case, unaffected by ch05 §5.7's new tracking.
    scpp::Program program = expect_parse_ok("extern \"C\" int f(int* p);");
    const scpp::Type& param_type = program.functions[0].params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer, "plain_pointer_defaults_to_mutable_pointee: should be Pointer");
    expect(param_type.is_mutable_pointee, "plain_pointer_defaults_to_mutable_pointee: is_mutable_pointee should be true");
}

// ch05/ch06: `const T name = expr;` -- a bare (non-reference,
// non-pointer) `const`-qualified local -- parses and sets Stmt::is_const,
// distinct from `const T&`/`const T*`'s own, separately-tracked
// read-only-ness (Type::is_mutable_ref/is_mutable_pointee).
void test_const_local_variable_parses() {
    scpp::Program program = expect_parse_ok("int f() { const int x = 5; return x; }");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "const_local_variable_parses: statement 0 should be VarDecl");
    expect(decl.is_const, "const_local_variable_parses: is_const should be true");
    expect(is_named_type(decl.type, "int"), "const_local_variable_parses: type should be plain 'int'");
}

// A plain (non-const) local must default to is_const == false -- the
// overwhelmingly common case, unaffected by the new tracking above.
void test_ordinary_local_variable_is_not_const() {
    scpp::Program program = expect_parse_ok("int f() { int x = 5; return x; }");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(!decl.is_const, "ordinary_local_variable_is_not_const: is_const should be false");
}

// ch05/ch06: a `const` local can never be given a value afterward, so
// omitting its initializer entirely (unlike an ordinary local, which may
// be declared bare) is rejected right at parse time.
void test_const_local_variable_without_initializer_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { const int x; return x; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "const_local_variable_without_initializer_is_rejected: expected a ParseError");
}

// Phase A: `constexpr` / `consteval` syntax is now represented directly in
// the AST so later constant-evaluation phases can consume it.
void test_constexpr_function_parses() {
    scpp::Program program = expect_parse_ok("constexpr int answer() { return 42; }\n");
    expect(program.functions[0].eval_mode == scpp::FunctionEvalMode::Constexpr,
           "constexpr_function_parses: eval_mode should be Constexpr");
}

void test_consteval_function_parses() {
    scpp::Program program = expect_parse_ok("consteval int answer() { return 42; }\n");
    expect(program.functions[0].eval_mode == scpp::FunctionEvalMode::Consteval,
           "consteval_function_parses: eval_mode should be Consteval");
}

void test_constexpr_constructor_parses() {
    scpp::Program program = expect_parse_ok("class Box { public: constexpr Box(int v) { value = v; } int value; };\n");
    const scpp::Function* ctor = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Box_new") ctor = &fn;
    }
    expect(ctor != nullptr, "constexpr_constructor_parses: expected synthesized constructor function");
    if (ctor) {
        expect(ctor->eval_mode == scpp::FunctionEvalMode::Constexpr,
               "constexpr_constructor_parses: ctor eval_mode should be Constexpr");
    }
}

void test_constexpr_local_variable_parses() {
    scpp::Program program = expect_parse_ok("int f() { constexpr int x = 5; return x; }\n");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "constexpr_local_variable_parses: statement 0 should be VarDecl");
    expect(decl.is_constexpr, "constexpr_local_variable_parses: is_constexpr should be true");
    expect(!decl.is_const, "constexpr_local_variable_parses: is_const should stay false");
}

void test_constexpr_local_variable_without_initializer_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f() { constexpr int x; return x; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "constexpr_local_variable_without_initializer_is_rejected: expected a ParseError");
}

void test_if_consteval_parses() {
    scpp::Program program = expect_parse_ok("int f() { if consteval { return 1; } else { return 2; } }\n");
    const scpp::Stmt& stmt = *program.functions[0].body->statements[0];
    expect(stmt.kind == scpp::StmtKind::If, "if_consteval_parses: statement 0 should be If");
    expect(stmt.if_mode == scpp::IfMode::ConstevalTrue, "if_consteval_parses: if_mode should be ConstevalTrue");
    expect(stmt.else_branch != nullptr, "if_consteval_parses: else branch should be present");
}

void test_if_not_consteval_parses() {
    scpp::Program program = expect_parse_ok("int f() { if !consteval { return 1; } else { return 2; } }\n");
    const scpp::Stmt& stmt = *program.functions[0].body->statements[0];
    expect(stmt.if_mode == scpp::IfMode::ConstevalFalse,
           "if_not_consteval_parses: if_mode should be ConstevalFalse");
}

// ch11 §11.3: `export module name;` marks a primary interface unit.
void test_export_module_declaration() {
    scpp::Program program = expect_parse_ok("export module std;\n");
    expect(program.module_name == "std", "export_module_declaration: module_name should be 'std'");
    expect(program.is_module_interface, "export_module_declaration: should be an interface unit");
    expect(!program.is_module_impl, "export_module_declaration: should not be an implementation unit");
}

// ch11 §11.3: a dotted module name (`org.lotx.cmath`) is read segment by
// segment (Identifier Dot Identifier ...), matching real module-name
// syntax, not namespace `::` syntax.
void test_dotted_module_name_declaration() {
    scpp::Program program = expect_parse_ok("export module org.lotx.cmath;\n");
    expect(program.module_name == "org.lotx.cmath", "dotted_module_name_declaration: expected 'org.lotx.cmath'");
}

// ch11 §11.3: `module name;` (no `export`) is an implementation unit.
void test_plain_module_declaration_is_implementation_unit() {
    scpp::Program program = expect_parse_ok("module std;\n");
    expect(program.module_name == "std", "plain_module_declaration_is_implementation_unit: module_name should be 'std'");
    expect(!program.is_module_interface,
           "plain_module_declaration_is_implementation_unit: should not be an interface unit");
    expect(program.is_module_impl, "plain_module_declaration_is_implementation_unit: should be an implementation unit");
}

void test_global_module_fragment_before_interface_module_declaration() {
    scpp::Program program = expect_parse_ok("module;\nexport module std;\n");
    expect(program.module_name == "std",
           "global_module_fragment_before_interface_module_declaration: module_name should be 'std'");
    expect(program.is_module_interface,
           "global_module_fragment_before_interface_module_declaration: should remain an interface unit");
}

void test_global_module_fragment_before_partition_declaration() {
    scpp::Program program = expect_parse_ok("module;\nexport module mylib.math:trig;\n");
    expect(program.module_name == "mylib.math",
           "global_module_fragment_before_partition_declaration: expected 'mylib.math'");
    expect(program.partition_name == "trig",
           "global_module_fragment_before_partition_declaration: expected 'trig'");
    expect(program.is_module_interface,
           "global_module_fragment_before_partition_declaration: should be an interface partition");
}

void test_global_module_fragment_without_following_module_declaration_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("module;\nint main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw,
           "global_module_fragment_without_following_module_declaration_is_rejected: expected a ParseError");
}

// A file with no module declaration at all is unaffected -- module_name
// stays empty, matching every scpp file before this chapter.
void test_no_module_declaration_leaves_module_name_empty() {
    scpp::Program program = expect_parse_ok("int main() { return 0; }");
    expect(program.module_name.empty(), "no_module_declaration_leaves_module_name_empty: module_name should be empty");
    expect(!program.is_module_interface && !program.is_module_impl,
           "no_module_declaration_leaves_module_name_empty: neither interface nor impl unit");
}

// ch11 §11.4: `namespace std { ... }` qualifies every nested
// declaration's name with the namespace prefix, and records
// namespace_path separately.
void test_namespace_qualifies_struct_name() {
    scpp::Program program = expect_parse_ok("namespace std { struct Point { int x; }; }");
    expect(program.structs.size() == 1, "namespace_qualifies_struct_name: expected 1 struct");
    const scpp::StructDef& def = program.structs[0];
    expect(def.name == "std::Point", "namespace_qualifies_struct_name: name should be 'std::Point'");
    expect(def.namespace_path.size() == 1 && def.namespace_path[0] == "std",
           "namespace_qualifies_struct_name: namespace_path should be ['std']");
}

// ch11 §11.4: the C++17 one-line nested namespace form (`namespace
// a::b { ... }`) records every segment in namespace_path, in order.
void test_nested_namespace_one_liner_qualifies_function_name() {
    scpp::Program program = expect_parse_ok("namespace a::b { int f() { return 0; } }");
    expect(program.functions.size() == 1, "nested_namespace_one_liner: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.name == "a::b::f", "nested_namespace_one_liner: name should be 'a::b::f'");
    expect(fn.namespace_path.size() == 2 && fn.namespace_path[0] == "a" && fn.namespace_path[1] == "b",
           "nested_namespace_one_liner: namespace_path should be ['a', 'b']");
}

// A namespace-qualified type reference (`std::Point`) resolves once the
// declaration itself has already registered its fully-qualified name.
void test_qualified_type_reference_parses() {
    scpp::Program program = expect_parse_ok("namespace std { struct Point { int x; }; }\n"
                     "int use_it() { std::Point p{}; return p.x; }");
    expect(program.functions.size() == 1, "qualified_type_reference_parses: expected 1 function");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "qualified_type_reference_parses: expected a VarDecl");
    expect(is_named_type(decl.type, "std::Point"), "qualified_type_reference_parses: type should be 'std::Point'");
}

// ch11 §11.3: `export` prefixing a top-level function marks it exported.
void test_export_prefix_marks_function_exported() {
    scpp::Program program = expect_parse_ok("export module std;\nnamespace std { export int f() { return 0; } }");
    expect(program.functions.size() == 1, "export_prefix_marks_function_exported: expected 1 function");
    expect(program.functions[0].is_exported, "export_prefix_marks_function_exported: should be exported");
}

// A declaration with no `export` prefix defaults to not-exported, even
// inside a namespace.
void test_no_export_prefix_leaves_function_not_exported() {
    scpp::Program program = expect_parse_ok("namespace std { int f() { return 0; } }");
    expect(!program.functions[0].is_exported, "no_export_prefix_leaves_function_not_exported: should not be exported");
}

// ch11 §11.3: `export { ... }` groups several declarations under one
// export marker, equivalent to prefixing each individually.
void test_export_group_marks_multiple_declarations_exported() {
    scpp::Program program = expect_parse_ok(
        "export module std;\n"
        "namespace std { export { int f() { return 0; } int g() { return 1; } } }");
    expect(program.functions.size() == 2, "export_group_marks_multiple_declarations_exported: expected 2 functions");
    expect(program.functions[0].is_exported && program.functions[1].is_exported,
           "export_group_marks_multiple_declarations_exported: both should be exported");
}

// `export namespace ns { ... }` is sugar for exporting the direct
// declarations inside the namespace block.
void test_export_namespace_block_marks_direct_members_exported() {
    scpp::Program program = expect_parse_ok(
        "export module demo;\n"
        "export namespace demo {\n"
        "    int f() { return 0; }\n"
        "    class Box { public: Box() { return; } };\n"
        "}\n");
    expect(program.functions.size() == 2,
           "export_namespace_block_marks_direct_members_exported: expected 2 functions including ctor");
    expect(program.classes.size() == 1, "export_namespace_block_marks_direct_members_exported: expected 1 class");
    expect(program.functions[0].is_exported && program.functions[1].is_exported,
           "export_namespace_block_marks_direct_members_exported: functions should be exported");
    expect(program.classes[0].is_exported,
           "export_namespace_block_marks_direct_members_exported: class should be exported");
}

// Whole-namespace export is equivalent to spelling `export` on each direct
// declaration individually.
void test_export_namespace_block_matches_per_declaration_exports() {
    scpp::Program block_program = expect_parse_ok(
        "export module demo;\n"
        "export namespace demo {\n"
        "    int f() { return 0; }\n"
        "    class Box { public: Box() { return; } };\n"
        "}\n");
    scpp::Program per_decl_program = expect_parse_ok(
        "export module demo;\n"
        "namespace demo {\n"
        "    export int f() { return 0; }\n"
        "    export class Box { public: Box() { return; } };\n"
        "}\n");
    expect(block_program.functions.size() == per_decl_program.functions.size(),
           "export_namespace_block_matches_per_declaration_exports: function counts should match");
    expect(block_program.classes.size() == per_decl_program.classes.size(),
           "export_namespace_block_matches_per_declaration_exports: class counts should match");
    expect(block_program.functions[0].name == per_decl_program.functions[0].name &&
               block_program.functions[0].is_exported == per_decl_program.functions[0].is_exported,
           "export_namespace_block_matches_per_declaration_exports: primary function export should match");
    expect(block_program.classes[0].name == per_decl_program.classes[0].name &&
               block_program.classes[0].is_exported == per_decl_program.classes[0].is_exported,
           "export_namespace_block_matches_per_declaration_exports: class export should match");
}

// ch11 §11.3: `export class Name { ... };` exports the whole class --
// every synthesized method inherits is_exported, not just the class
// name entry itself.
void test_export_class_propagates_to_methods() {
    scpp::Program program = expect_parse_ok(
        "export module std;\n"
        "namespace std { export class Point { public: Point() { return; } }; }");
    expect(program.classes.size() == 1, "export_class_propagates_to_methods: expected 1 class");
    expect(program.classes[0].is_exported, "export_class_propagates_to_methods: class itself should be exported");
    expect(program.classes[0].name == "std::Point", "export_class_propagates_to_methods: name should be 'std::Point'");
    expect(program.functions.size() == 1, "export_class_propagates_to_methods: expected 1 synthesized ctor");
    expect(program.functions[0].name == "std::Point_new",
           "export_class_propagates_to_methods: ctor should be named 'std::Point_new'");
    expect(program.functions[0].is_exported, "export_class_propagates_to_methods: ctor should inherit is_exported");
}

// Exported declarations may live in any namespace; export validity is not
// tied to the module's dotted name.
void test_export_in_non_matching_namespace_is_allowed() {
    scpp::Program program = expect_parse_ok("export module std;\nnamespace other { export int f() { return 0; } }");
    expect(program.functions.size() == 1, "export_in_non_matching_namespace_is_allowed: expected 1 function");
    expect(program.functions[0].is_exported, "export_in_non_matching_namespace_is_allowed: should be exported");
}

// Global-scope exports are also allowed in a module interface.
void test_export_with_no_namespace_is_allowed() {
    scpp::Program program = expect_parse_ok("export module std;\nexport int f() { return 0; }");
    expect(program.functions.size() == 1, "export_with_no_namespace_is_allowed: expected 1 function");
    expect(program.functions[0].is_exported, "export_with_no_namespace_is_allowed: should be exported");
}

// Namespaces that happen to nest under the module name still continue to
// work; they're just no longer special-cased.
void test_export_in_deeper_nested_namespace_is_allowed() {
    scpp::Program program = expect_parse_ok(
        "export module org.lotx.cmath;\n"
        "namespace org::lotx::cmath::trig { export int f() { return 0; } }");
    expect(program.functions.size() == 1, "export_in_deeper_nested_namespace_is_allowed: expected 1 function");
    expect(program.functions[0].is_exported, "export_in_deeper_nested_namespace_is_allowed: should be exported");
}

// ch11 §11.3: `export` on a declaration in a file with no module
// declaration at all has nothing to export from -- rejected.
void test_export_without_any_module_declaration_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("namespace std { export int f() { return 0; } }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "export_without_any_module_declaration_is_rejected: expected a ParseError");
}

// ch11 §11.6: a bare `extern` (no `"C"` string) declaration has ordinary
// scpp linkage -- a bodyless declaration distinct from `extern "C"`
// (is_extern_c stays false, is_module_extern is set instead).
void test_bare_extern_declaration_is_module_extern() {
    scpp::Program program = expect_parse_ok("extern int square(int x);");
    expect(program.functions.size() == 1, "bare_extern_declaration_is_module_extern: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_module_extern, "bare_extern_declaration_is_module_extern: is_module_extern should be true");
    expect(!fn.is_extern_c, "bare_extern_declaration_is_module_extern: is_extern_c should be false");
    expect(fn.body == nullptr, "bare_extern_declaration_is_module_extern: body should be null (bodyless declaration)");
}

// A bare `extern` declaration is namespace-qualified like any ordinary
// scpp-linkage declaration (unlike `extern "C"`, which never is).
void test_bare_extern_declaration_is_namespace_qualified() {
    scpp::Program program = expect_parse_ok("namespace org::lotx::cmath { extern int sqrt(int x); }");
    expect(program.functions.size() == 1, "bare_extern_declaration_is_namespace_qualified: expected 1 function");
    expect(program.functions[0].name == "org::lotx::cmath::sqrt",
           "bare_extern_declaration_is_namespace_qualified: expected qualified name");
}

void test_module_forward_declarations_reconcile_to_definitions() {
    scpp::Program program = expect_parse_ok(
        "export module mathlib;\n"
        "namespace mathlib {\n"
        "    export int is_even(int x);\n"
        "    int is_odd(int x);\n"
        "    int is_even(int x) { if (x == 0) return 1; return is_odd(x - 1); }\n"
        "    int is_odd(int x) { if (x == 0) return 0; return is_even(x - 1); }\n"
        "}\n");
    expect(program.functions.size() == 2,
           "module_forward_declarations_reconcile_to_definitions: expected only the two definitions");
    const scpp::Function* even = nullptr;
    const scpp::Function* odd = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "mathlib::is_even") even = &fn;
        if (fn.name == "mathlib::is_odd") odd = &fn;
    }
    expect(even != nullptr, "module_forward_declarations_reconcile_to_definitions: expected mathlib::is_even");
    expect(odd != nullptr, "module_forward_declarations_reconcile_to_definitions: expected mathlib::is_odd");
    expect(even != nullptr && even->body != nullptr,
           "module_forward_declarations_reconcile_to_definitions: is_even should keep its definition body");
    expect(odd != nullptr && odd->body != nullptr,
           "module_forward_declarations_reconcile_to_definitions: is_odd should keep its definition body");
    expect(even != nullptr && even->is_exported,
           "module_forward_declarations_reconcile_to_definitions: export on the forward declaration should survive");
    expect(odd != nullptr && !odd->is_exported,
           "module_forward_declarations_reconcile_to_definitions: non-exported helper should stay private");
}

void test_module_forward_declaration_mismatched_definition_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "export module mathlib;\n"
            "namespace mathlib {\n"
            "    export int value(int x);\n"
            "    int value(char x) { return x; }\n"
            "}\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "module_forward_declaration_mismatched_definition_is_rejected: expected a ParseError");
}

// ch11 §11.4: `export module name:part;` declares an interface
// partition -- module_name stays just the base dotted name, with the
// part after ':' recorded separately in partition_name.
void test_partition_declaration_sets_partition_name() {
    scpp::Program program = expect_parse_ok("export module mylib.math:trig;\n");
    expect(program.module_name == "mylib.math", "partition_declaration_sets_partition_name: expected 'mylib.math'");
    expect(program.partition_name == "trig", "partition_declaration_sets_partition_name: expected 'trig'");
    expect(program.is_module_interface, "partition_declaration_sets_partition_name: should be an interface partition");
}

// ch11 §11.4: `module name:part;` (no `export`) declares an
// implementation partition.
void test_implementation_partition_declaration() {
    scpp::Program program = expect_parse_ok("module mylib.math:detail;\n");
    expect(program.module_name == "mylib.math", "implementation_partition_declaration: expected 'mylib.math'");
    expect(program.partition_name == "detail", "implementation_partition_declaration: expected 'detail'");
    expect(!program.is_module_interface, "implementation_partition_declaration: should not be an interface partition");
    expect(program.is_module_impl, "implementation_partition_declaration: should be an implementation partition");
}

// ch11 §11.4: `import :part;` inside a file with no module declaration
// of its own makes no sense -- partitions only exist within a module.
void test_partition_import_outside_module_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("import :trig;\nint main() { return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "partition_import_outside_module_is_rejected: expected a ParseError");
}

// ch11 §11.4: `import :part;` without a partition resolver configured
// (mirrors the existing cross-module "no module resolver" check) is
// rejected with a clear error rather than crashing.
void test_partition_import_without_resolver_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("export module mylib.math;\nimport :trig;\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "partition_import_without_resolver_is_rejected: expected a ParseError");
}

// ch11 §11.4: a partition import (`import :part;`) resolves via
// PartitionResolver, keyed as "<module_name>:<partition>" -- merging
// every declaration (exported or not) *with* their bodies, unlike a
// cross-module import.
void test_partition_import_merges_with_body() {
    scpp::PartitionResolver partition_resolver = [](const std::string& key) -> scpp::Program {
        expect(key == "mylib.math:trig", "partition_import_merges_with_body: expected key 'mylib.math:trig'");
        auto result = scpp::parse(
            "export module mylib.math:trig;\n"
            "namespace mylib::math {\n"
            "    export int sin_deg_approx(int degrees) { return degrees / 2; }\n"
            "    int private_helper(int x) { return x; }\n"
            "}\n");
        if (!result.has_value()) throw std::move(result).error();
        return std::move(result.value());
    };
    scpp::Program program = expect_parse_ok(
        "export module mylib.math;\n"
        "export import :trig;\n"
        "namespace mylib::math { export int square(int x) { return x * x; } }\n",
        /*resolver=*/{}, partition_resolver);
    // 2 functions from the partition (sin_deg_approx + private_helper)
    // plus this file's own square.
    expect(program.functions.size() == 3, "partition_import_merges_with_body: expected 3 functions");
    for (const scpp::Function& fn : program.functions) {
        expect(fn.body != nullptr, "partition_import_merges_with_body: '" + fn.name + "' should keep its body");
        expect(fn.owning_module.empty(),
               "partition_import_merges_with_body: '" + fn.name + "' owning_module should stay empty");
    }
    bool found_exported_sin = false;
    bool found_private_helper = false;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "mylib::math::sin_deg_approx") {
            found_exported_sin = true;
            expect(fn.is_exported, "partition_import_merges_with_body: sin_deg_approx should be exported "
                                    "(export import re-exports the partition's own exports)");
        }
        if (fn.name == "mylib::math::private_helper") {
            found_private_helper = true;
            expect(!fn.is_exported,
                   "partition_import_merges_with_body: private_helper was never exported by the partition "
                   "itself, so it should stay unexported after merging");
        }
    }
    expect(found_exported_sin, "partition_import_merges_with_body: expected to find sin_deg_approx");
    expect(found_private_helper, "partition_import_merges_with_body: expected to find private_helper");
}

// ch11 §11.4: a plain `import :part;` (no `export`) merges the
// partition's declarations for internal use, but forces is_exported
// false on all of them regardless of the partition's own markings --
// they must not leak to an external importer of the whole module.
void test_plain_partition_import_does_not_reexport() {
    scpp::PartitionResolver partition_resolver = [](const std::string&) -> scpp::Program {
        auto result = scpp::parse(
            "export module mylib.math:trig;\n"
            "namespace mylib::math { export int sin_deg_approx(int degrees) { return degrees / 2; } }\n");
        if (!result.has_value()) throw std::move(result).error();
        return std::move(result.value());
    };
    scpp::Program program = expect_parse_ok(
        "export module mylib.math;\n"
        "import :trig;\n"
        "namespace mylib::math { export int square(int x) { return x * x; } }\n",
        /*resolver=*/{}, partition_resolver);
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "mylib::math::sin_deg_approx") {
            expect(!fn.is_exported, "plain_partition_import_does_not_reexport: sin_deg_approx should not be "
                                     "exported (plain import :part; never re-exports)");
        }
    }
}

// ch11 §11.4: `export import :part;` on an implementation partition
// (declared via `module name:part;`, no `export`) is a compile error --
// such a partition can never export anything to the outside, by
// construction.
void test_export_import_on_implementation_partition_is_rejected() {
    scpp::PartitionResolver partition_resolver = [](const std::string&) -> scpp::Program {
        auto result = scpp::parse("module mylib.math:detail;\nnamespace mylib::math { export int f() { return 0; } }\n");
        if (!result.has_value()) throw std::move(result).error();
        return std::move(result.value());
    };
    bool threw = false;
    if (auto _r = scpp::parse("export module mylib.math;\nexport import :detail;\n", /*resolver=*/{}, partition_resolver); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "export_import_on_implementation_partition_is_rejected: expected a ParseError");
}

// ch03: `T&&` (rvalue reference) is parsed only in a function parameter's
// declared type -- `is_rvalue_ref` set, `is_mutable_ref` also true (an
// rvalue-reference parameter is always fully mutable/ownable inside the
// callee).
void test_rvalue_reference_parameter_parses() {
    scpp::Program program = expect_parse_ok("int take(int&& x) { return x; }");
    expect(program.functions.size() == 1, "rvalue_reference_parameter_parses: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 1, "rvalue_reference_parameter_parses: expected 1 param");
    const scpp::Type& type = fn.params[0].type;
    expect(type.kind == scpp::TypeKind::Reference, "rvalue_reference_parameter_parses: kind should be Reference");
    expect(type.is_rvalue_ref, "rvalue_reference_parameter_parses: is_rvalue_ref should be true");
    expect(type.is_mutable_ref, "rvalue_reference_parameter_parses: is_mutable_ref should also be true");
    expect(is_named_type(*type.pointee, "int"), "rvalue_reference_parameter_parses: pointee should be 'int'");
}

// ch03: a plain `T&`/`const T&` parameter must still parse with
// is_rvalue_ref left false -- this flag must not accidentally default to
// true or leak across unrelated parameters.
void test_ordinary_reference_parameter_is_not_rvalue_ref() {
    scpp::Program program = expect_parse_ok("int take(int& x, const int& y) { return x + y; }");
    const scpp::Function& fn = program.functions[0];
    expect(!fn.params[0].type.is_rvalue_ref, "ordinary_reference_parameter_is_not_rvalue_ref: 'int&' param");
    expect(!fn.params[1].type.is_rvalue_ref, "ordinary_reference_parameter_is_not_rvalue_ref: 'const int&' param");
}

// ch03: `const T&&` is rejected -- a moved-from value must be mutable to
// move *from*, so `const` can never qualify an rvalue reference.
void test_const_rvalue_reference_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int take(const int&& x) { return x; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "const_rvalue_reference_is_rejected: expected a ParseError");
}

// ch03: `T&&` is scoped to a function/method/constructor parameter's
// declared type only (ch03's own table says "T&& (parameter)") --
// rejected for a local variable declaration, a class field, a function's
// return type, and a nested type argument (std::unique_ptr<T>'s own T).
void test_rvalue_reference_rejected_outside_parameter_position() {
    auto expect_rejected = [](const std::string& source, const char* label) {
        bool threw = false;
        if (source.find("import std;") != std::string::npos) {
            if (auto _r = try_parse_with_std_imports(source); !_r.has_value()) threw = true;
        } else {
            if (auto _r = scpp::parse(source); !_r.has_value()) threw = true;
        }
        expect(threw, std::string("rvalue_reference_rejected_outside_parameter_position: ") + label);
    };
    expect_rejected("int f() { int&& x = 5; return 0; }", "var decl");
    expect_rejected("int&& f() { return 5; }", "return type");
    expect_rejected(
        "class Widget {\n"
        "public:\n"
        "    Widget() {}\n"
        "    int&& field;\n"
        "};\n",
        "class field");
    expect_rejected("import std;\nint f() { std::unique_ptr<int&&> p{}; return 0; }", "unique_ptr element type");
}

// ch05 §5.11: `template<typename T> concept Name = requires(...) { ...
// };` with a *compound* requirement (`{ expr } -> std::same_as<T>;`)
// synthesizes a hidden witness class (ClassDef::is_concept_witness) with
// one bodyless method per requirement, named via the same
// `ClassName_memberName` scheme every other method uses -- so the
// return type and parameter (receiver) shape are exactly what an
// ordinary method's would be.
void test_concept_compound_requirement_synthesizes_witness_class() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept Shape = requires(const T& t) {\n"
        "    { t.area() } -> std::same_as<int>;\n"
        "};\n"
        "int main() { return 0; }\n");
    expect(program.concepts.size() == 1, "concept_compound_requirement_synthesizes_witness_class: expected 1 concept");
    const scpp::ConceptDef& def = program.concepts[0];
    expect(def.name == "Shape", "concept_compound_requirement_synthesizes_witness_class: name should be 'Shape'");
    expect(def.template_param_name == "T",
           "concept_compound_requirement_synthesizes_witness_class: template_param_name should be 'T'");
    expect(def.requires_param_name == "t",
           "concept_compound_requirement_synthesizes_witness_class: requires_param_name should be 't'");
    expect(def.requirements.size() == 1, "concept_compound_requirement_synthesizes_witness_class: expected 1 requirement");
    expect(def.requirements[0].method_name == "area",
           "concept_compound_requirement_synthesizes_witness_class: method_name should be 'area'");
    expect(def.requirements[0].has_return_constraint,
           "concept_compound_requirement_synthesizes_witness_class: has_return_constraint should be true");
    expect(is_named_type(def.requirements[0].return_type, "int"),
           "concept_compound_requirement_synthesizes_witness_class: return_type should be 'int'");

    bool found_witness_class = false;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Shape") {
            found_witness_class = true;
            expect(c.is_concept_witness,
                   "concept_compound_requirement_synthesizes_witness_class: ClassDef should be is_concept_witness");
        }
    }
    expect(found_witness_class, "concept_compound_requirement_synthesizes_witness_class: expected a witness ClassDef");

    bool found_witness_method = false;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Shape_area") {
            found_witness_method = true;
            expect(fn.body == nullptr,
                   "concept_compound_requirement_synthesizes_witness_class: witness method should be bodyless");
            expect(fn.params.size() == 1,
                   "concept_compound_requirement_synthesizes_witness_class: witness method should have 1 param (this)");
            expect(fn.params[0].name == "this",
                   "concept_compound_requirement_synthesizes_witness_class: witness method's param 0 should be 'this'");
            expect(!fn.params[0].type.is_mutable_ref,
                   "concept_compound_requirement_synthesizes_witness_class: 'this' should be const ('const T& t')");
            expect(is_named_type(fn.return_type, "int"),
                   "concept_compound_requirement_synthesizes_witness_class: witness method return type should be 'int'");
        }
    }
    expect(found_witness_method, "concept_compound_requirement_synthesizes_witness_class: expected a witness method "
                                  "'Shape_area'");
}

// ch05 §5.11: a *simple* requirement (no braces, no `->`) directly
// invoking the placeholder itself (`f(x);`, e.g. IntConsumer) is modeled
// as a call to a fixed synthesized method name ("call") -- shared with a
// closure's own compiler-synthesized operator() (ch05 §5.12), so both
// resolve through the same "bare Call redirects to a method call" sugar.
void test_concept_simple_direct_invocation_requirement_synthesizes_call_method() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept IntConsumer = requires(T f, int x) { f(x); };\n"
        "int main() { return 0; }\n");
    expect(program.concepts.size() == 1,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected 1 concept");
    const scpp::ConceptDef& def = program.concepts[0];
    expect(def.requires_param_name == "f",
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: requires_param_name should be 'f'");
    expect(!def.requires_param_is_const,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: requires parameter should not be const");
    expect(def.requirements.size() == 1,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected 1 requirement");
    expect(def.requirements[0].method_name == "call",
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: method_name should be 'call'");
    expect(!def.requirements[0].has_return_constraint,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: simple requirement has no return "
           "constraint");
    expect(def.requirements[0].arg_types.size() == 1,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected 1 arg type");
    expect(is_named_type(def.requirements[0].arg_types[0], "int"),
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: arg type should be 'int'");

    bool found_witness_method = false;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "IntConsumer_call") {
            found_witness_method = true;
            expect(fn.params.size() == 2,
                   "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected 2 params "
                   "(this + x)");
            expect(!fn.params[0].type.is_mutable_ref,
                   "concept_simple_direct_invocation_requirement_synthesizes_call_method: witness 'this' should be "
                   "read-only-capable so later concrete instantiations, not the abstract witness, decide const "
                   "receiver validity");
            expect(is_named_type(fn.params[1].type, "int"),
                   "concept_simple_direct_invocation_requirement_synthesizes_call_method: param 1 should be 'int'");
        }
    }
    expect(found_witness_method,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected a witness method "
           "'IntConsumer_call'");
}

// ch05 §5.11: a requirement's expression must be shaped as a call on the
// concept's own requires-parameter -- an unrelated identifier is
// rejected (v0.1 does not support an arbitrary requirement expression).
void test_concept_requirement_on_wrong_receiver_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) {\n"
            "    { other.area() } -> std::same_as<int>;\n"
            "};\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "concept_requirement_on_wrong_receiver_is_rejected: expected a ParseError");
}

// ch05 §5.11: a compound requirement's constraint must be
// `std::same_as<T>` -- `std::convertible_to<T>` is rejected outright
// (scpp has no implicit scalar conversions at all, so the two concepts
// would mean the same thing anyway).
void test_concept_convertible_to_constraint_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) {\n"
            "    { t.area() } -> std::convertible_to<int>;\n"
            "};\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "concept_convertible_to_constraint_is_rejected: expected a ParseError");
}

// ch05 §5.11: a requirement's call argument must be a bare reference to
// one of the requires-expression's *other* (non-placeholder) parameters
// -- an unknown identifier is rejected.
void test_concept_requirement_unknown_argument_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("template<typename T>\nconcept IntConsumer = requires(T f, int x) { f(y); };\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "concept_requirement_unknown_argument_is_rejected: expected a ParseError");
}

// ch11 §11.5: `export` on a concept declaration, like every other
// top-level declaration, has no effect (is rejected) outside a module
// file.
void test_export_concept_outside_module_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("export template<typename T>\nconcept Shape = requires(const T& t) { t.area(); };\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "export_concept_outside_module_is_rejected: expected a ParseError");
}

// ch11 §11.4/§11.5: a concept declared inside `namespace a { ... }`
// namespace-qualifies its own name and its witness class/method exactly
// like a struct/class/function would.
void test_concept_inside_namespace_is_qualified() {
    scpp::Program program = expect_parse_ok(
        "export module shapes;\n"
        "namespace shapes {\n"
        "export template<typename T>\n"
        "concept Shape = requires(const T& t) { t.area(); };\n"
        "}\n");
    expect(program.concepts.size() == 1, "concept_inside_namespace_is_qualified: expected 1 concept");
    expect(program.concepts[0].name == "shapes::Shape",
           "concept_inside_namespace_is_qualified: name should be namespace-qualified");
    expect(program.concepts[0].is_exported, "concept_inside_namespace_is_qualified: should be exported");
    bool found = false;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "shapes::Shape_area") found = true;
    }
    expect(found, "concept_inside_namespace_is_qualified: expected witness method 'shapes::Shape_area'");
}

// ch05 §5.11: the abbreviated generic-function parameter form `const
// ConceptName auto& name` parses to an ordinary Reference parameter
// whose innermost Named type is the concept's own witness-class name --
// Param::generic_concept records which concept produced it, and the
// enclosing Function is marked is_generic_template.
void test_generic_parameter_const_auto_ref_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept Shape = requires(const T& t) { t.area(); };\n"
        "int print_area(const Shape auto& s) { return 0; }\n");
    const scpp::Function* print_area = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "print_area") print_area = &fn;
    }
    expect(print_area != nullptr, "generic_parameter_const_auto_ref_parses: expected function 'print_area'");
    expect(print_area->is_generic_template,
           "generic_parameter_const_auto_ref_parses: 'print_area' should be is_generic_template");
    expect(print_area->params.size() == 1, "generic_parameter_const_auto_ref_parses: expected 1 param");
    const scpp::Param& param = print_area->params[0];
    expect(param.generic_concept == "Shape", "generic_parameter_const_auto_ref_parses: generic_concept should be "
                                              "'Shape'");
    expect(param.type.kind == scpp::TypeKind::Reference,
           "generic_parameter_const_auto_ref_parses: kind should be Reference");
    expect(!param.type.is_mutable_ref, "generic_parameter_const_auto_ref_parses: should be a shared reference "
                                        "('const ... &')");
    expect(!param.type.is_rvalue_ref, "generic_parameter_const_auto_ref_parses: should not be an rvalue reference");
    expect(is_named_type(*param.type.pointee, "Shape"),
           "generic_parameter_const_auto_ref_parses: pointee should name the witness class 'Shape'");
}

// ch05 §5.11: `ConceptName auto&&` parses with the raw `&&` spelling
// preserved in Type::is_rvalue_ref; later call resolution may collapse it
// like a forwarding reference when deduction binds it to an lvalue.
void test_generic_parameter_auto_rvalue_ref_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept InvocableRvalue = requires(T f, int x) { f(x); };\n"
        "int for_each_doubled(InvocableRvalue auto&& f) { return 0; }\n");
    const scpp::Function* fn_ptr = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "for_each_doubled") fn_ptr = &fn;
    }
    expect(fn_ptr != nullptr, "generic_parameter_auto_rvalue_ref_parses: expected function 'for_each_doubled'");
    expect(fn_ptr->is_generic_template,
           "generic_parameter_auto_rvalue_ref_parses: should be is_generic_template");
    const scpp::Param& param = fn_ptr->params[0];
    expect(param.generic_concept == "InvocableRvalue",
           "generic_parameter_auto_rvalue_ref_parses: generic_concept should be 'InvocableRvalue'");
    expect(param.type.kind == scpp::TypeKind::Reference,
           "generic_parameter_auto_rvalue_ref_parses: kind should be Reference");
    expect(param.type.is_rvalue_ref, "generic_parameter_auto_rvalue_ref_parses: is_rvalue_ref should be true");
    expect(is_named_type(*param.type.pointee, "InvocableRvalue"),
           "generic_parameter_auto_rvalue_ref_parses: pointee should name the witness class");
}

// ch05 §5.11: `ConceptName auto&` (mutable, no leading const) parses as
// a mutable-reference generic parameter.
void test_generic_parameter_mutable_auto_ref_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept Shape = requires(const T& t) { t.area(); };\n"
        "int touch(Shape auto& s) { return 0; }\n");
    const scpp::Function& fn = program.functions[program.functions.size() - 1];
    expect(fn.name == "touch", "generic_parameter_mutable_auto_ref_parses: expected function 'touch'");
    expect(fn.params[0].type.is_mutable_ref,
           "generic_parameter_mutable_auto_ref_parses: should be a mutable reference");
    expect(!fn.params[0].type.is_rvalue_ref,
           "generic_parameter_mutable_auto_ref_parses: should not be an rvalue reference");
}

// ch05 §5.11: bare `auto` (no concept name at all) parses as its own
// generic parameter form -- "the parameter's type is treated as fully
// opaque... exactly as if it were constrained by a concept whose
// requires-expression guarantees nothing". Recorded under the reserved
// "$auto" witness (parse_program's own comment), never a real,
// user-spellable concept name.
void test_bare_auto_parameter_parses() {
    scpp::Program program = expect_parse_ok("int identity(auto x) { return 0; }\n");
    const scpp::Function* identity_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "identity") identity_fn = &fn;
    }
    expect(identity_fn != nullptr, "bare_auto_parameter_parses: expected function 'identity'");
    expect(identity_fn->is_generic_template, "bare_auto_parameter_parses: should be is_generic_template");
    expect(identity_fn->params.size() == 1, "bare_auto_parameter_parses: expected 1 param");
    const scpp::Param& param = identity_fn->params[0];
    expect(param.generic_concept == "$auto", "bare_auto_parameter_parses: generic_concept should be '$auto'");
    expect(is_named_type(param.type, "$auto"), "bare_auto_parameter_parses: type should name the '$auto' witness");
}

// ch05 §5.12: real C++14 generic lambdas -- a bare `auto` lambda
// parameter (unlike a *named*-concept-constrained one, still rejected)
// -- now parses instead of failing with "expected a type name". Lambda
// resolution (to a real class + "call" method) only happens later, in
// movecheck's Monomorphizer pass, not at parse time (see
// test_lambda_with_explicit_captures_parses' own "name should be empty
// before resolution" check) -- so this inspects the raw Lambda
// expression's own lambda_params directly, exactly like that test does.
void test_bare_auto_lambda_parameter_parses() {
    scpp::Program program = expect_parse_ok("int main() { return [](auto x) { return 0; }(1); }\n");
    const scpp::Function& main_fn = program.functions[0];
    const scpp::Expr& call_expr = *main_fn.body->statements[0]->expr;
    expect(call_expr.kind == scpp::ExprKind::Call, "bare_auto_lambda_parameter_parses: expected Call");
    expect(call_expr.lhs != nullptr && call_expr.lhs->kind == scpp::ExprKind::Lambda,
           "bare_auto_lambda_parameter_parses: expected a Lambda IIFE receiver");
    const scpp::Expr& lambda = *call_expr.lhs;
    expect(lambda.lambda_params.size() == 1, "bare_auto_lambda_parameter_parses: expected 1 param");
    expect(lambda.lambda_params[0].generic_concept == "$auto",
           "bare_auto_lambda_parameter_parses: generic_concept should be '$auto'");
}

// ch05 §5.11: an identifier immediately followed by `auto` that does
// *not* name a declared concept is rejected with a clear error (rather
// than silently mis-parsing or crashing).
void test_generic_parameter_unknown_concept_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int f(NotAConcept auto& x) { return 0; }"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "generic_parameter_unknown_concept_is_rejected: expected a ParseError");
}

// ch05 §5.11: a generic (concept-constrained) parameter is only
// supported on a free function in this version -- rejected on a method
// or constructor.
void test_generic_parameter_on_method_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) { t.area(); };\n"
            "class Widget {\n"
            "public:\n"
            "    Widget() { return; }\n"
            "    int touch(Shape auto& s) { return 0; }\n"
            "};\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "generic_parameter_on_method_is_rejected: expected a ParseError");
}

// ch05 §5.12: `[capture-list](params) { body }` parses to a raw,
// unresolved Lambda Expr -- explicit captures (by-value, by-reference)
// recorded verbatim, params reusing the same shared parameter-list
// parser methods/constructors use, and no synthesized class name yet
// (that's movecheck's closure-resolution pass's job, see
// Expr::name's own comment).
void test_lambda_with_explicit_captures_parses() {
    scpp::Program program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    int y = 10;\n"
        "    apply([x, &y](int z) -> int { return x + y + z; }, 3);\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    expect(main_fn != nullptr, "lambda_with_explicit_captures_parses: expected function 'main'");
    // main's body: VarDecl x, VarDecl y, ExprStmt(Call apply(...)), Return.
    const scpp::Stmt& call_stmt = *main_fn->body->statements[2];
    expect(call_stmt.kind == scpp::StmtKind::ExprStmt, "lambda_with_explicit_captures_parses: expected ExprStmt");
    const scpp::Expr& call_expr = *call_stmt.expr;
    expect(call_expr.kind == scpp::ExprKind::Call, "lambda_with_explicit_captures_parses: expected Call");
    expect(call_expr.args.size() == 2, "lambda_with_explicit_captures_parses: expected 2 args");
    const scpp::Expr& lambda = *call_expr.args[0];
    expect(lambda.kind == scpp::ExprKind::Lambda, "lambda_with_explicit_captures_parses: expected Lambda");
    expect(lambda.name.empty(), "lambda_with_explicit_captures_parses: name (synthesized class) should be empty "
                                 "before resolution");
    expect(lambda.lambda_blanket_mode == scpp::LambdaCaptureMode::None,
           "lambda_with_explicit_captures_parses: expected no blanket mode");
    expect(lambda.lambda_captures.size() == 2, "lambda_with_explicit_captures_parses: expected 2 captures");
    expect(lambda.lambda_captures[0].name == "x" && !lambda.lambda_captures[0].by_reference,
           "lambda_with_explicit_captures_parses: capture 0 should be by-value 'x'");
    expect(lambda.lambda_captures[1].name == "y" && lambda.lambda_captures[1].by_reference,
           "lambda_with_explicit_captures_parses: capture 1 should be by-reference 'y'");
    expect(lambda.lambda_params.size() == 1 && lambda.lambda_params[0].name == "z",
           "lambda_with_explicit_captures_parses: expected 1 param 'z'");
    expect(lambda.has_lambda_explicit_return_type && is_named_type(lambda.type, "int"),
           "lambda_with_explicit_captures_parses: expected explicit return type 'int'");
    expect(!lambda.lambda_is_mutable, "lambda_with_explicit_captures_parses: should not be mutable");
    expect(lambda.lambda_body != nullptr, "lambda_with_explicit_captures_parses: expected a body");
}

// ch05 §5.12: `[=]`/`[&]` (blanket captures) parse with no explicit
// capture entries and lambda_blanket_mode set -- free-variable
// resolution happens later (movecheck's closure-resolution pass), not
// here.
void test_lambda_blanket_capture_modes_parse() {
    scpp::Program value_program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    apply([=](int z) { return x + z; }, 3);\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : value_program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Expr& lambda1 = *main_fn->body->statements[1]->expr->args[0];
    expect(lambda1.lambda_blanket_mode == scpp::LambdaCaptureMode::ByValue,
           "lambda_blanket_capture_modes_parse: '[=]' should be ByValue");
    expect(lambda1.lambda_captures.empty(),
           "lambda_blanket_capture_modes_parse: '[=]' should have no explicit captures");

    scpp::Program ref_program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    apply([&](int z) { return x + z; }, 3);\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn2 = nullptr;
    for (const scpp::Function& fn : ref_program.functions) {
        if (fn.name == "main") main_fn2 = &fn;
    }
    const scpp::Expr& lambda2 = *main_fn2->body->statements[1]->expr->args[0];
    expect(lambda2.lambda_blanket_mode == scpp::LambdaCaptureMode::ByReference,
           "lambda_blanket_capture_modes_parse: '[&]' should be ByReference");

    // Mixed: `[&, x]` -- blanket by-reference, 'x' explicitly by-value.
    scpp::Program mixed_program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    int y = 10;\n"
        "    apply([&, x](int z) { return x + y + z; }, 3);\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn3 = nullptr;
    for (const scpp::Function& fn : mixed_program.functions) {
        if (fn.name == "main") main_fn3 = &fn;
    }
    const scpp::Expr& lambda3 = *main_fn3->body->statements[2]->expr->args[0];
    expect(lambda3.lambda_blanket_mode == scpp::LambdaCaptureMode::ByReference,
           "lambda_blanket_capture_modes_parse: '[&, x]' should be ByReference blanket");
    expect(lambda3.lambda_captures.size() == 1 && lambda3.lambda_captures[0].name == "x" &&
               !lambda3.lambda_captures[0].by_reference,
           "lambda_blanket_capture_modes_parse: '[&, x]' should explicitly list 'x' by-value");
}

// ch05 §5.12: an init-capture (`[name = expr]`) records the init
// expression -- how a move-only type crosses into a closure, e.g.
// `[p = std::move(p)]`.
void test_lambda_init_capture_parses() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int apply(int x, int y) { return x; }\n"
        "int main() {\n"
        "    std::unique_ptr<int> p = std::make_unique<int>(5);\n"
        "    apply([q = std::move(p)](int z) { return z; }, 3);\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Expr& lambda = *main_fn->body->statements[1]->expr->args[0];
    expect(lambda.lambda_captures.size() == 1, "lambda_init_capture_parses: expected 1 capture");
    expect(lambda.lambda_captures[0].name == "q", "lambda_init_capture_parses: capture name should be 'q'");
    expect(lambda.lambda_captures[0].init != nullptr, "lambda_init_capture_parses: expected a non-null init expr");
    expect(lambda.lambda_captures[0].init->kind == scpp::ExprKind::Move,
           "lambda_init_capture_parses: init expr should be a Move");
}

// ch05 §5.12: `[this]` captures a reference to the enclosing method's
// own receiver, while `[*this]` captures the enclosing object by value.
void test_lambda_this_and_star_this_captures_parse() {
    scpp::Program program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "class Widget {\n"
        "public:\n"
        "    Widget() { return; }\n"
        "    int use_lambda() {\n"
        "        return apply([this](int z) { return z; }, 3);\n"
        "    }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* method = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Widget_use_lambda") method = &fn;
    }
    expect(method != nullptr, "lambda_this_and_star_this_captures_parse: expected 'Widget_use_lambda'");
    const scpp::Expr& lambda = *method->body->statements[0]->expr->args[0];
    expect(lambda.lambda_captures.size() == 1 && lambda.lambda_captures[0].name == "this" &&
               lambda.lambda_captures[0].by_reference,
           "lambda_this_and_star_this_captures_parse: expected 1 capture '&this'");

    scpp::Program star_program = expect_parse_ok(
        "int apply(int x, int y) { return x; }\n"
        "class Widget {\n"
        "public:\n"
        "    Widget() { return; }\n"
        "    int use_lambda() {\n"
        "        return apply([*this](int z) { return z; }, 3);\n"
        "    }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* star_method = nullptr;
    for (const scpp::Function& fn : star_program.functions) {
        if (fn.name == "Widget_use_lambda") star_method = &fn;
    }
    expect(star_method != nullptr, "lambda_this_and_star_this_captures_parse: expected star-this method");
    const scpp::Expr& star_lambda = *star_method->body->statements[0]->expr->args[0];
    expect(star_lambda.lambda_captures.size() == 1 && star_lambda.lambda_captures[0].name == "this" &&
              !star_lambda.lambda_captures[0].by_reference,
           "lambda_this_and_star_this_captures_parse: expected 1 capture '*this'");
}

void test_function_pointer_declarators_parse() {
    scpp::Program program = expect_parse_ok(
        "struct Op { int (*fn)(int, int); };\n"
        "int add(int a, int b) { return a + b; }\n"
        "int main() {\n"
        "    int (*fp)(int, int) = add;\n"
        "    int (* [[scpp::unsafe]] up)(int, int) = add;\n"
        "    Op op{};\n"
        "    op.fn = fp;\n"
        "    return op.fn(2, 3) + (*up)(1, 1);\n"
        "}\n");
    expect(program.structs.size() == 1, "function_pointer_declarators_parse: expected 1 struct");
    expect(program.structs[0].fields.size() == 1, "function_pointer_declarators_parse: expected 1 field");
    const scpp::Type& field_type = program.structs[0].fields[0].type;
    expect(field_type.kind == scpp::TypeKind::FunctionPointer,
           "function_pointer_declarators_parse: struct field should be a function pointer");
    expect(field_type.function_return != nullptr && is_named_type(*field_type.function_return, "int"),
           "function_pointer_declarators_parse: field return type should be 'int'");
    expect(field_type.function_params.size() == 2 && is_named_type(field_type.function_params[0], "int") &&
               is_named_type(field_type.function_params[1], "int"),
           "function_pointer_declarators_parse: field params should be '(int, int)'");
    const scpp::Function& main_fn = program.functions[1];
    expect(main_fn.body->statements[0]->type.kind == scpp::TypeKind::FunctionPointer,
           "function_pointer_declarators_parse: local fp should be a function pointer");
    expect(main_fn.body->statements[1]->type.kind == scpp::TypeKind::FunctionPointer &&
               main_fn.body->statements[1]->type.is_unsafe_function_pointer,
           "function_pointer_declarators_parse: local up should be unsafe-qualified function pointer");
}

// ch05 §5.12: a lambda's own parameter list does not support a
// concept-constrained ("ConceptName auto") parameter in this version --
// mirrors the same restriction on methods/constructors.
void test_lambda_generic_parameter_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) { t.area(); };\n"
            "int apply(int x) { return x; }\n"
            "int main() {\n"
            "    apply([](Shape auto& s) { return 0; });\n"
            "    return 0;\n"
            "}\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "lambda_generic_parameter_is_rejected: expected a ParseError");
}

// ch05 §5.12: `mutable` parses and sets lambda_is_mutable.
void test_lambda_mutable_keyword_parses() {
    scpp::Program program = expect_parse_ok(
        "int apply(int x) { return x; }\n"
        "int main() {\n"
        "    int x = 5;\n"
        "    apply([x](int z) mutable { return x + z; });\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Expr& lambda = *main_fn->body->statements[1]->expr->args[0];
    expect(lambda.lambda_is_mutable, "lambda_mutable_keyword_parses: expected lambda_is_mutable true");
}

// ch05 §5.14: `template<typename T> class Name { ... };` -- a bare
// (unconstrained) type parameter, legal for a class (never a struct,
// see the next test). Registers ClassDef::template_params and marks it
// a template; the type parameter's own bare name ("T") is only a
// temporary type name scoped to this one declaration's own body, so a
// field/param typed "T" parses as an ordinary Named type.
void test_generic_class_bare_type_param_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Vec {\n"
        "    T item;\n"
        "public:\n"
        "    void push(T x) { this.item = x; return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* vec = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Vec") vec = &c;
    }
    expect(vec != nullptr, "generic_class_bare_type_param_parses: expected a ClassDef named 'Vec'");
    expect(vec->template_params.size() == 1,
           "generic_class_bare_type_param_parses: expected exactly 1 template param");
    expect(vec->template_params[0].name == "T", "generic_class_bare_type_param_parses: param name should be 'T'");
    expect(vec->template_params[0].concept_name.empty(),
           "generic_class_bare_type_param_parses: bare param's concept_name should be empty");
    expect(vec->fields.size() == 1 && vec->fields[0].type.kind == scpp::TypeKind::Named &&
               vec->fields[0].type.name == "T",
           "generic_class_bare_type_param_parses: field 'item' should be Named('T')");
}

void test_generic_class_multiple_type_params_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename First, typename Second>\n"
        "class Pair {\n"
        "    First first;\n"
        "    Second second;\n"
        "public:\n"
        "    const First& left() const { return this.first; }\n"
        "    const Second& right() const { return this.second; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* pair = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Pair") pair = &c;
    }
    expect(pair != nullptr, "generic_class_multiple_type_params_parse: expected a ClassDef named 'Pair'");
    expect(pair->template_params.size() == 2,
           "generic_class_multiple_type_params_parse: expected exactly 2 template params");
    expect(pair->template_params[0].name == "First" && pair->template_params[1].name == "Second",
           "generic_class_multiple_type_params_parse: param names should be 'First' and 'Second'");
    expect(pair->fields.size() == 2 && pair->fields[0].type.kind == scpp::TypeKind::Named &&
               pair->fields[0].type.name == "First" && pair->fields[1].type.kind == scpp::TypeKind::Named &&
               pair->fields[1].type.name == "Second",
           "generic_class_multiple_type_params_parse: fields should preserve both template parameter types");
}

void test_generic_class_named_pack_method_params_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename R, typename... Args>\n"
        "class Invoker {\n"
        "public:\n"
        "    R call(Args... args) { return 0; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* call_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Invoker_call") call_fn = &fn;
    }
    expect(call_fn != nullptr, "generic_class_named_pack_method_params_parse: expected 'Invoker_call'");
    expect(call_fn->params.size() == 2, "generic_class_named_pack_method_params_parse: expected this + one pack param");
    expect(call_fn->params[1].is_parameter_pack, "generic_class_named_pack_method_params_parse: method param should be a pack");
    expect(call_fn->params[1].type.kind == scpp::TypeKind::Named && call_fn->params[1].type.name == "Args",
           "generic_class_named_pack_method_params_parse: pack element type should be Named('Args')");
}

void test_generic_class_named_pack_function_pointer_params_parse() {
    scpp::Program program = expect_parse_ok(
        "template<typename R, typename... Args>\n"
        "class Invoker {\n"
        "    R (*fp)(Args...);\n"
        "public:\n"
        "    int arity() const { return 0; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* invoker = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Invoker") invoker = &c;
    }
    expect(invoker != nullptr, "generic_class_named_pack_function_pointer_params_parse: expected ClassDef 'Invoker'");
    expect(invoker->fields.size() == 1, "generic_class_named_pack_function_pointer_params_parse: expected one field");
    expect(invoker->fields[0].type.kind == scpp::TypeKind::FunctionPointer,
           "generic_class_named_pack_function_pointer_params_parse: field should be a function pointer");
    expect(invoker->fields[0].type.function_params.size() == 1 &&
               invoker->fields[0].type.function_params[0].kind == scpp::TypeKind::Named &&
               invoker->fields[0].type.function_params[0].name == "Args" &&
               invoker->fields[0].type.function_params[0].is_pack_expansion,
           "generic_class_named_pack_function_pointer_params_parse: function pointer should carry an Args... pack expansion");
}

void test_class_member_templates_parse() {
    scpp::Program program = expect_parse_ok(
        "class Sink {\n"
        "public:\n"
        "    template<typename T>\n"
        "    Sink(T&& x) { return; }\n"
        "    template<typename T>\n"
        "    int call(T&& x) { return 0; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* ctor = nullptr;
    const scpp::Function* call = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Sink_new") ctor = &fn;
        if (fn.name == "Sink_call") call = &fn;
    }
    expect(ctor != nullptr && !ctor->template_params.empty() && ctor->template_params[0].name == "T",
           "class_member_templates_parse: expected templated constructor 'Sink_new'");
    expect(call != nullptr && !call->template_params.empty() && call->template_params[0].name == "T",
           "class_member_templates_parse: expected templated method 'Sink_call'");
}

void test_function_type_template_argument_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename Sig>\n"
        "class Holder {\n"
        "public:\n"
        "    int value;\n"
        "};\n"
        "int main() { Holder<int(int, int)> h{}; return 0; }\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    expect(main_fn != nullptr && main_fn->body != nullptr, "function_type_template_argument_parses: expected main body");
    const scpp::Stmt* decl = main_fn->body->statements[0].get();
    expect(decl->kind == scpp::StmtKind::VarDecl, "function_type_template_argument_parses: expected first stmt var decl");
    expect(decl->type.kind == scpp::TypeKind::Named && decl->type.name == "Holder" && decl->type.template_args.size() == 1,
           "function_type_template_argument_parses: expected Holder<...> type");
    expect(decl->type.template_args[0].kind == scpp::TypeKind::Function &&
               decl->type.template_args[0].function_params.size() == 2 &&
               decl->type.template_args[0].function_return &&
               decl->type.template_args[0].function_return->kind == scpp::TypeKind::Named &&
               decl->type.template_args[0].function_return->name == "int",
           "function_type_template_argument_parses: expected int(int, int) function type argument");
}

void test_qualified_function_type_template_argument_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename Sig>\n"
        "class Holder {\n"
        "public:\n"
        "    int value;\n"
        "};\n"
        "int main() { Holder<int(int) const &&> h{}; return 0; }\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    expect(main_fn != nullptr && main_fn->body != nullptr,
           "qualified_function_type_template_argument_parses: expected main body");
    const scpp::Stmt* decl = main_fn->body->statements[0].get();
    expect(decl->kind == scpp::StmtKind::VarDecl && decl->type.template_args.size() == 1,
           "qualified_function_type_template_argument_parses: expected Holder<...> var decl");
    const scpp::Type& sig = decl->type.template_args[0];
    expect(sig.kind == scpp::TypeKind::Function && sig.is_const_function &&
               sig.function_ref_qualifier == scpp::ReceiverRefQualifier::RValue,
           "qualified_function_type_template_argument_parses: expected const && function type argument");
}

void test_ref_qualified_methods_parse() {
    scpp::Program program = expect_parse_ok(
        "class Callable {\n"
        "public:\n"
        "    int call() & { return 1; }\n"
        "    int call() && { return 2; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* lvalue = nullptr;
    const scpp::Function* rvalue = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name != "Callable_call") continue;
        if (fn.receiver_ref_qualifier == scpp::ReceiverRefQualifier::LValue) lvalue = &fn;
        if (fn.receiver_ref_qualifier == scpp::ReceiverRefQualifier::RValue) rvalue = &fn;
    }
    expect(lvalue != nullptr, "ref_qualified_methods_parse: expected lvalue-qualified overload");
    expect(rvalue != nullptr, "ref_qualified_methods_parse: expected rvalue-qualified overload");
}

void test_explicit_template_function_designator_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "int thunk(T x) { return x; }\n"
        "int main() {\n"
        "    int (*fp)(int) = thunk<int>;\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    expect(main_fn != nullptr && main_fn->body != nullptr,
           "explicit_template_function_designator_parses: expected main body");
    const scpp::Stmt* decl = main_fn->body->statements[0].get();
    expect(decl->kind == scpp::StmtKind::VarDecl && decl->init != nullptr &&
               decl->init->kind == scpp::ExprKind::Identifier &&
               decl->init->explicit_template_args.size() == 1,
           "explicit_template_function_designator_parses: expected identifier with one explicit template arg");
}

void test_const_qualified_template_type_argument_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Box { public: virtual ~Box() { return; } };\n"
        "int main() {\n"
        "    Box<const int> box{};\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr && main_fn->body != nullptr,
           "const_qualified_template_type_argument_parses: expected main body");
    const scpp::Stmt* decl = main_fn->body->statements[0].get();
    expect(decl->kind == scpp::StmtKind::VarDecl && decl->type.kind == scpp::TypeKind::Named &&
               decl->type.template_args.size() == 1,
           "const_qualified_template_type_argument_parses: expected Box with one template arg");
    expect(decl->type.template_args[0].kind == scpp::TypeKind::Named &&
               decl->type.template_args[0].name == "int" &&
               decl->type.template_args[0].is_const_qualified,
           "const_qualified_template_type_argument_parses: expected template arg 'const int'");
}

void test_const_qualified_explicit_template_argument_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "int thunk(T x) { return x; }\n"
        "int main() {\n"
        "    int (*fp)(int) = thunk<const int>;\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr && main_fn->body != nullptr,
           "const_qualified_explicit_template_argument_parses: expected main body");
    const scpp::Stmt* decl = main_fn->body->statements[0].get();
    expect(decl->kind == scpp::StmtKind::VarDecl && decl->init != nullptr &&
               decl->init->explicit_template_args.size() == 1 &&
               decl->init->explicit_template_args[0].is_type,
           "const_qualified_explicit_template_argument_parses: expected one explicit type arg");
    expect(decl->init->explicit_template_args[0].type.kind == scpp::TypeKind::Named &&
               decl->init->explicit_template_args[0].type.name == "int" &&
               decl->init->explicit_template_args[0].type.is_const_qualified,
           "const_qualified_explicit_template_argument_parses: expected explicit arg 'const int'");
}

void test_global_qualified_call_parses() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    return ::foo::bar();\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr && main_fn->body != nullptr,
           "global_qualified_call_parses: expected main body");
    const scpp::Stmt* ret = main_fn->body->statements[0].get();
    expect(ret->kind == scpp::StmtKind::Return && ret->expr != nullptr &&
               ret->expr->kind == scpp::ExprKind::Call,
           "global_qualified_call_parses: expected return call");
    expect(ret->expr->name == "foo::bar",
           "global_qualified_call_parses: expected joined qualified name");
    expect(ret->expr->explicit_global_qualification,
           "global_qualified_call_parses: expected explicit global qualification flag");
}

void test_class_partial_specialization_on_function_type_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename Sig>\n"
        "class Holder;\n"
        "template<typename R, typename... Args>\n"
        "class Holder<R(Args...)> {\n"
        "public:\n"
        "    R (*fn_)(Args...);\n"
        "    R call(Args... args) { return this.fn_(args...); }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* forward = nullptr;
    const scpp::ClassDef* partial = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name != "Holder") continue;
        if (c.is_forward_declaration) forward = &c;
        if (c.is_partial_specialization) partial = &c;
    }
    expect(forward != nullptr,
           "class_partial_specialization_on_function_type_parses: expected primary forward declaration");
    expect(partial != nullptr, "class_partial_specialization_on_function_type_parses: expected partial specialization");
    expect(partial->specialization_template_args.size() == 1 &&
               partial->specialization_template_args[0].kind == scpp::TypeKind::Function,
           "class_partial_specialization_on_function_type_parses: expected one function-type specialization arg");
    expect(partial->specialization_template_args[0].function_params.size() == 1 &&
               partial->specialization_template_args[0].function_params[0].is_pack_expansion &&
               partial->specialization_template_args[0].function_params[0].name == "Args",
           "class_partial_specialization_on_function_type_parses: expected trailing Args... pack in function pattern");
    const scpp::Function* call_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Holder__" + partial->template_owner_id + "_call" &&
            fn.generic_method_owner_id == partial->template_owner_id) {
            call_fn = &fn;
        }
    }
    expect(call_fn != nullptr,
           "class_partial_specialization_on_function_type_parses: expected owner-id-qualified Holder call");
    expect(call_fn->params.size() == 2 && call_fn->params[1].is_parameter_pack,
           "class_partial_specialization_on_function_type_parses: expected call's Args... parameter pack");
}

void test_variadic_specialization_member_names_include_owner_id() {
    scpp::Program program = expect_parse_ok("template<typename... Ts> class Box;\n"
                                         "template<> class Box<> {\n"
                                         "public:\n"
                                         "    Box(const char* s) { return; }\n"
                                         "    int size() const { return 0; }\n"
                                         "};\n"
                                         "template<typename Head, typename... Tail>\n"
                                         "class Box<Head, Tail...> {\n"
                                         "public:\n"
                                         "    Box(const char* s) { return; }\n"
                                         "    int size() const { return 0; }\n"
                                         "};\n"
                                         "int main() { return 0; }\n");
    const scpp::ClassDef* base_case = nullptr;
    const scpp::ClassDef* recursive_case = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name != "Box" || !c.is_variadic_specialization) continue;
        if (c.template_params.empty()) {
            base_case = &c;
        } else {
            recursive_case = &c;
        }
    }
    expect(base_case != nullptr && recursive_case != nullptr,
           "variadic_specialization_member_names_include_owner_id: expected both variadic specializations");
    bool found_base_ctor = false;
    bool found_base_method = false;
    bool found_recursive_ctor = false;
    bool found_recursive_method = false;
    for (const scpp::Function& fn : program.functions) {
        if (base_case != nullptr && fn.name == "Box__" + base_case->template_owner_id + "_new") found_base_ctor = true;
        if (base_case != nullptr && fn.name == "Box__" + base_case->template_owner_id + "_size") found_base_method = true;
        if (recursive_case != nullptr && fn.name == "Box__" + recursive_case->template_owner_id + "_new") {
            found_recursive_ctor = true;
        }
        if (recursive_case != nullptr && fn.name == "Box__" + recursive_case->template_owner_id + "_size") {
            found_recursive_method = true;
        }
    }
    expect(found_base_ctor && found_base_method && found_recursive_ctor && found_recursive_method,
           "variadic_specialization_member_names_include_owner_id: expected owner-id-qualified ctor/method names");
}

// ch05 §5.14: a method may layer its own `requires Concept<T>` clause,
// recorded on Function::method_requires_concept -- independent of
// whether the class's own type parameter is itself bare or constrained.
void test_generic_class_method_requires_clause_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept Describable = requires(const T& t) {\n"
        "    { t.magnitude() } -> std::same_as<int>;\n"
        "};\n"
        "template<typename T>\n"
        "class Vec {\n"
        "    T item;\n"
        "public:\n"
        "    void push(T x) { this.item = x; return; }\n"
        "    int describe() const requires Describable<T> { return 0; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* push_fn = nullptr;
    const scpp::Function* describe_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "Vec_push") push_fn = &fn;
        if (fn.name == "Vec_describe") describe_fn = &fn;
    }
    expect(push_fn != nullptr, "generic_class_method_requires_clause_parses: expected 'Vec_push'");
    expect(push_fn->method_requires_concept.empty(),
           "generic_class_method_requires_clause_parses: 'push' should have no requires clause");
    expect(describe_fn != nullptr, "generic_class_method_requires_clause_parses: expected 'Vec_describe'");
    expect(describe_fn->method_requires_concept == "Describable",
           "generic_class_method_requires_clause_parses: 'describe' should require 'Describable'");
}

// ch05 §5.14: a generic struct's own type parameter must be concept-
// constrained (`template<Concept T> struct Name { ... };`) -- a bare
// one is a parse error, since struct field triviality (ch04 §4.1) is a
// whole-type property no per-member clause could decompose the way a
// class's own methods can.
void test_generic_struct_concept_constrained_type_param_parses() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "concept Describable = requires(const T& t) {\n"
        "    { t.magnitude() } -> std::same_as<int>;\n"
        "};\n"
        "template<Describable T>\n"
        "struct Wrapper {\n"
        "    T item;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::StructDef* wrapper = nullptr;
    for (const scpp::StructDef& s : program.structs) {
        if (s.name == "Wrapper") wrapper = &s;
    }
    expect(wrapper != nullptr, "generic_struct_concept_constrained_type_param_parses: expected a StructDef named 'Wrapper'");
    expect(wrapper->template_params.size() == 1,
           "generic_struct_concept_constrained_type_param_parses: expected exactly 1 template param");
    expect(wrapper->template_params[0].concept_name == "Describable",
           "generic_struct_concept_constrained_type_param_parses: concept_name should be 'Describable'");
}

void test_generic_struct_bare_type_param_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "template<typename T>\n"
            "struct Pair {\n"
            "    T x;\n"
            "};\n"
            "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "generic_struct_bare_type_param_is_rejected: expected a ParseError");
}

// ch05 §5.14: `Name<Arg>` (a generic-type instantiation) parses to a
// Type whose `name` still names the *template* and whose `template_args`
// holds the (single, v0.1-only) concrete argument -- left unresolved
// for movecheck's Monomorphizer (see Type::template_args' own comment).
void test_generic_type_instantiation_parses_with_template_args() {
    scpp::Program program = expect_parse_ok(
        "template<typename T>\n"
        "class Vec {\n"
        "    T item;\n"
        "};\n"
        "int main() {\n"
        "    Vec<int> v{};\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Stmt& var_decl = *main_fn->body->statements[0];
    expect(var_decl.type.kind == scpp::TypeKind::Named && var_decl.type.name == "Vec",
           "generic_type_instantiation_parses_with_template_args: type name should still be 'Vec'");
    expect(var_decl.type.template_args.size() == 1,
           "generic_type_instantiation_parses_with_template_args: expected exactly 1 template arg");
    expect(is_named_type(var_decl.type.template_args[0], "int"),
           "generic_type_instantiation_parses_with_template_args: template arg should be Named('int')");
}

// ch05 §5.14: `class Derived : public Base { ... };` records one
// BaseSpecifier on the ClassDef.
void test_class_public_inheritance_parses() {
    scpp::Program program = expect_parse_ok(
        "class Animal {\n"
        "public:\n"
        "    Animal() { return; }\n"
        "};\n"
        "class Dog : public Animal {\n"
        "public:\n"
        "    Dog() { return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* dog = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Dog") dog = &c;
    }
    expect(dog != nullptr, "class_public_inheritance_parses: expected a ClassDef named 'Dog'");
    expect(dog->base_specifiers.size() == 1, "class_public_inheritance_parses: expected exactly one base specifier");
    expect(dog->base_specifiers[0].base_type.name == "Animal",
           "class_public_inheritance_parses: base type name should be 'Animal'");
    expect(dog->base_specifiers[0].access == scpp::AccessSpecifier::Public,
           "class_public_inheritance_parses: base access should be Public");
}

// ch05 §5.14: `class Derived : Base { ... };` (no access keyword)
// defaults to private inheritance, matching real C++'s own default for
// `class` (unlike `struct`, which defaults to public -- but structs
// have no inheritance here at all).
void test_class_inheritance_defaults_to_private() {
    scpp::Program program = expect_parse_ok(
        "class Animal {\n"
        "public:\n"
        "    Animal() { return; }\n"
        "};\n"
        "class Dog : Animal {\n"
        "public:\n"
        "    Dog() { return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* dog = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Dog") dog = &c;
    }
    expect(dog != nullptr, "class_inheritance_defaults_to_private: expected a ClassDef named 'Dog'");
    expect(dog->base_specifiers.size() == 1, "class_inheritance_defaults_to_private: expected exactly one base specifier");
    expect(dog->base_specifiers[0].access == scpp::AccessSpecifier::Private,
           "class_inheritance_defaults_to_private: base_access should default to Private");
}

// ch05 §5.14: a base class must already be a declared class (single-
// pass parsing) -- referencing an undeclared name is a ParseError.
void test_class_inheritance_from_undeclared_class_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "class Dog : public Animal {\n"
            "public:\n"
            "    Dog() { return; }\n"
            "};\n"
            "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "class_inheritance_from_undeclared_class_is_rejected: expected a ParseError");
}

void test_interface_attribute_sets_class_flag() {
    scpp::Program program = expect_parse_ok(
        "class [[scpp::interface]] IReader {\n"
        "public:\n"
        "    IReader() { return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* iface = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "IReader") iface = &c;
    }
    expect(iface != nullptr, "interface_attribute_sets_class_flag: expected a ClassDef named 'IReader'");
    expect(iface != nullptr && iface->is_interface,
           "interface_attribute_sets_class_flag: expected [[scpp::interface]] to set ClassDef::is_interface");
}

void test_multiple_base_specifiers_parse_with_access_and_virtual_flags() {
    scpp::Program program = expect_parse_ok(
        "class Base1 { public: Base1() { return; } };\n"
        "class [[scpp::interface]] IFoo { public: virtual ~IFoo() = default; };\n"
        "class [[scpp::interface]] IBar { public: virtual ~IBar() = default; };\n"
        "class Derived : public Base1, public virtual IFoo, private virtual IBar {\n"
        "public:\n"
        "    Derived() { return; }\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* derived = find_class_named(program, "Derived");
    expect(derived != nullptr,
           "multiple_base_specifiers_parse_with_access_and_virtual_flags: expected a ClassDef named 'Derived'");
    expect(derived != nullptr && derived->base_specifiers.size() == 3,
           "multiple_base_specifiers_parse_with_access_and_virtual_flags: expected 3 base specifiers");
    expect(derived != nullptr && derived->base_specifiers[0].base_type.name == "Base1" &&
               derived->base_specifiers[0].access == scpp::AccessSpecifier::Public &&
               !derived->base_specifiers[0].is_virtual &&
               derived->base_specifiers[0].kind == scpp::BaseClassKind::OrdinaryClass,
           "multiple_base_specifiers_parse_with_access_and_virtual_flags: expected public non-virtual Base1");
    expect(derived != nullptr && derived->base_specifiers[1].base_type.name == "IFoo" &&
               derived->base_specifiers[1].access == scpp::AccessSpecifier::Public &&
               derived->base_specifiers[1].is_virtual &&
               derived->base_specifiers[1].kind == scpp::BaseClassKind::Interface,
           "multiple_base_specifiers_parse_with_access_and_virtual_flags: expected public virtual IFoo");
    expect(derived != nullptr && derived->base_specifiers[2].base_type.name == "IBar" &&
               derived->base_specifiers[2].access == scpp::AccessSpecifier::Private &&
               derived->base_specifiers[2].is_virtual &&
               derived->base_specifiers[2].kind == scpp::BaseClassKind::Interface,
           "multiple_base_specifiers_parse_with_access_and_virtual_flags: expected private virtual IBar");
}

void test_class_scope_using_declaration_parses() {
    scpp::Program program = expect_parse_ok(
        "class Base {\n"
        "public:\n"
        "    void work() { return; }\n"
        "};\n"
        "class Derived : public Base {\n"
        "public:\n"
        "    using Base::work;\n"
        "private:\n"
        "    using Base::work;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::ClassDef* derived = find_class_named(program, "Derived");
    expect(derived != nullptr, "class_scope_using_declaration_parses: expected a ClassDef named 'Derived'");
    expect(derived != nullptr && derived->using_declarations.size() == 2,
           "class_scope_using_declaration_parses: expected 2 using declarations");
    expect(derived != nullptr && derived->using_declarations[0].base_name == "Base" &&
               derived->using_declarations[0].member_name == "work" &&
               derived->using_declarations[0].access == scpp::AccessSpecifier::Public,
           "class_scope_using_declaration_parses: expected public using Base::work");
    expect(derived != nullptr && derived->using_declarations[1].base_name == "Base" &&
               derived->using_declarations[1].member_name == "work" &&
               derived->using_declarations[1].access == scpp::AccessSpecifier::Private,
           "class_scope_using_declaration_parses: expected private using Base::work");
}

void test_virtual_override_pure_and_defaulted_member_flags_parse() {
    scpp::Program program = expect_parse_ok(
        "class [[scpp::interface]] IBase {\n"
        "public:\n"
        "    virtual ~IBase() = default;\n"
        "    virtual void run() = 0;\n"
        "};\n"
        "class Derived : public virtual IBase {\n"
        "public:\n"
        "    virtual ~Derived() override = default;\n"
        "    virtual void run() override;\n"
        "    virtual void helper() = 0;\n"
        "    Derived() = default;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* iface_dtor = find_function_named(program, "IBase_delete");
    const scpp::Function* iface_run = find_function_named(program, "IBase_run");
    const scpp::Function* derived_dtor = find_function_named(program, "Derived_delete");
    const scpp::Function* derived_run = find_function_named(program, "Derived_run");
    const scpp::Function* derived_helper = find_function_named(program, "Derived_helper");
    const scpp::Function* derived_ctor = find_function_named(program, "Derived_new");
    expect(iface_dtor != nullptr && iface_dtor->is_virtual && iface_dtor->is_defaulted,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected IBase destructor to be virtual and defaulted");
    expect(iface_run != nullptr && iface_run->is_virtual && iface_run->is_pure,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected IBase::run to be virtual and pure");
    expect(derived_dtor != nullptr && derived_dtor->is_virtual && derived_dtor->is_override &&
               derived_dtor->is_defaulted,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected Derived destructor virtual/override/defaulted");
    expect(derived_run != nullptr && derived_run->is_virtual && derived_run->is_override &&
               !derived_run->is_pure && !derived_run->is_defaulted,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected Derived::run virtual/override only");
    expect(derived_helper != nullptr && derived_helper->is_virtual && derived_helper->is_pure,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected Derived::helper virtual and pure");
    expect(derived_ctor != nullptr && derived_ctor->is_defaulted,
           "virtual_override_pure_and_defaulted_member_flags_parse: expected Derived constructor defaulted");
}

void test_override_without_virtual_member_flags_parse() {
    scpp::Program program = expect_parse_ok(
        "class Base {\n"
        "public:\n"
        "    virtual ~Base() = default;\n"
        "    virtual void run() = 0;\n"
        "    virtual int operator*() = 0;\n"
        "};\n"
        "class Derived : public Base {\n"
        "public:\n"
        "    ~Derived() override = default;\n"
        "    void run() override;\n"
        "    int operator*() override;\n"
        "};\n"
        "int main() { return 0; }\n");
    const scpp::Function* derived_dtor = find_function_named(program, "Derived_delete");
    const scpp::Function* derived_run = find_function_named(program, "Derived_run");
    const scpp::Function* derived_deref = find_function_named(program, "Derived_operator_deref");
    expect(derived_dtor != nullptr && derived_dtor->is_override && !derived_dtor->is_virtual &&
               derived_dtor->is_defaulted,
           "override_without_virtual_member_flags_parse: expected destructor override/defaulted without virtual flag");
    expect(derived_run != nullptr && derived_run->is_override && !derived_run->is_virtual &&
               !derived_run->is_pure && !derived_run->is_defaulted,
           "override_without_virtual_member_flags_parse: expected method override without virtual flag");
    expect(derived_deref != nullptr && derived_deref->is_override && !derived_deref->is_virtual &&
               !derived_deref->is_pure && !derived_deref->is_defaulted,
           "override_without_virtual_member_flags_parse: expected operator* override without virtual flag");
}

// ch05 §5.14: `template<typename... Ts> class Tuple;` -- a variadic
// primary template's own bodyless forward declaration -- marks the
// ClassDef is_variadic_primary_template, records its single pack
// parameter, and pushes no fields/base at all.
void test_variadic_primary_template_decl_parses() {
    scpp::Program program = expect_parse_ok("template<typename... Ts> class Tuple;\n"
                                         "int main() { return 0; }\n");
    const scpp::ClassDef* tuple = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Tuple") tuple = &c;
    }
    expect(tuple != nullptr, "variadic_primary_template_decl_parses: expected a ClassDef named 'Tuple'");
    expect(tuple->is_variadic_primary_template,
           "variadic_primary_template_decl_parses: is_variadic_primary_template should be true");
    expect(tuple->template_params.size() == 1 && tuple->template_params[0].is_pack,
           "variadic_primary_template_decl_parses: expected a single pack parameter 'Ts'");
    expect(tuple->fields.empty(), "variadic_primary_template_decl_parses: expected no fields");
}

// ch05 §5.14: `template<> class Tuple<> {};` -- the empty-pack base-case
// specialization of an already-declared variadic primary template.
void test_variadic_empty_pack_specialization_parses() {
    scpp::Program program = expect_parse_ok("template<typename... Ts> class Tuple;\n"
                                         "template<> class Tuple<> {};\n"
                                         "int main() { return 0; }\n");
    const scpp::ClassDef* base_case = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Tuple" && c.is_variadic_specialization) base_case = &c;
    }
    expect(base_case != nullptr, "variadic_empty_pack_specialization_parses: expected a specialization ClassDef");
    expect(base_case->template_params.empty(),
           "variadic_empty_pack_specialization_parses: expected zero template params");
    expect(base_case->fields.empty(), "variadic_empty_pack_specialization_parses: expected no fields");
}

// ch05 §5.14: `template<typename Head, typename... Tail> class
// Tuple<Head, Tail...> : private Tuple<Tail...> { Head head; };` -- the
// recursive-case specialization: records template_params (Head +
// Tail(is_pack)), the recursive base specifier (the pack spread
// as the base's sole argument), and the field typed by the head
// parameter.
void test_variadic_recursive_specialization_parses() {
    scpp::Program program = expect_parse_ok("template<typename... Ts> class Tuple;\n"
                                         "template<> class Tuple<> {};\n"
                                         "template<typename Head, typename... Tail>\n"
                                         "class Tuple<Head, Tail...> : private Tuple<Tail...> {\n"
                                         "    Head head;\n"
                                         "};\n"
                                         "int main() { return 0; }\n");
    const scpp::ClassDef* recursive_case = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "Tuple" && c.is_variadic_specialization && !c.template_params.empty()) recursive_case = &c;
    }
    expect(recursive_case != nullptr, "variadic_recursive_specialization_parses: expected a recursive-case ClassDef");
    expect(recursive_case->template_params.size() == 2 && recursive_case->template_params[0].name == "Head" &&
               recursive_case->template_params[1].name == "Tail" && recursive_case->template_params[1].is_pack,
           "variadic_recursive_specialization_parses: expected params [Head, Tail(pack)]");
    expect(recursive_case->base_specifiers.size() == 1,
           "variadic_recursive_specialization_parses: expected exactly one base specifier");
    expect(recursive_case->base_specifiers[0].base_type.name == "Tuple",
           "variadic_recursive_specialization_parses: base type name should be 'Tuple'");
    expect(recursive_case->base_specifiers[0].pack_arg_name == "Tail",
           "variadic_recursive_specialization_parses: pack_arg_name should be 'Tail'");
    expect(recursive_case->base_specifiers[0].base_type.template_args.size() == 1 &&
               recursive_case->base_specifiers[0].base_type.template_args[0].name == "Tail" &&
               recursive_case->base_specifiers[0].base_type.template_args[0].is_pack_expansion,
           "variadic_recursive_specialization_parses: expected base type template arg 'Tail...'");
    expect(recursive_case->fields.size() == 1 && recursive_case->fields[0].type.name == "Head",
           "variadic_recursive_specialization_parses: expected a single field typed 'Head'");
}

// ch05 §5.14: a use-site instantiation of a variadic generic type,
// `Tuple<int, bool, char>`, parses with all 3 concrete arguments
// recorded (in order) on Type::template_args.
void test_variadic_instantiation_with_multiple_args_parses() {
    scpp::Program program = expect_parse_ok("template<typename... Ts> class Tuple;\n"
                                         "template<> class Tuple<> {};\n"
                                         "template<typename Head, typename... Tail>\n"
                                         "class Tuple<Head, Tail...> : private Tuple<Tail...> {\n"
                                         "    Head head;\n"
                                         "};\n"
                                         "int main() {\n"
                                         "    Tuple<int, bool, char> t{};\n"
                                         "    return 0;\n"
                                         "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Stmt& var_decl = *main_fn->body->statements[0];
    expect(var_decl.type.name == "Tuple", "variadic_instantiation_with_multiple_args_parses: type name should be 'Tuple'");
    expect(var_decl.type.template_args.size() == 3,
           "variadic_instantiation_with_multiple_args_parses: expected exactly 3 template args");
    expect(is_named_type(var_decl.type.template_args[0], "int") &&
               is_named_type(var_decl.type.template_args[1], "bool") &&
               is_named_type(var_decl.type.template_args[2], "char"),
           "variadic_instantiation_with_multiple_args_parses: expected args [int, bool, char] in order");
}

// ch05 §5.14: a variadic generic type is class-only -- `struct` has no
// inheritance, so it cannot vary its own layout by arity.
void test_variadic_struct_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("template<typename... Ts> struct Tuple;\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "variadic_struct_is_rejected: expected a ParseError");
}

// ch05 §5.14: a specialization referencing an undeclared primary
// template is a ParseError.
void test_variadic_specialization_without_primary_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("template<> class Tuple<> {};\n"
                    "int main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "variadic_specialization_without_primary_is_rejected: expected a ParseError");
}

// ch05 §5.11: `template<typename T> T name(params) { body }` -- the full
// header form for a generic function (as opposed to the abbreviated
// `Concept auto` form) -- records Function::template_params and marks
// is_generic_template.
void test_generic_function_full_header_form_parses() {
    scpp::Program program = expect_parse_ok("template<typename T> T make() {\n"
                                         "    T x{};\n"
                                         "    return x;\n"
                                         "}\n"
                                         "int main() { return 0; }\n");
    const scpp::Function* make_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "make") make_fn = &fn;
    }
    expect(make_fn != nullptr, "generic_function_full_header_form_parses: expected a Function named 'make'");
    expect(make_fn->is_generic_template,
           "generic_function_full_header_form_parses: is_generic_template should be true");
    expect(make_fn->template_params.size() == 1 && make_fn->template_params[0].name == "T",
           "generic_function_full_header_form_parses: expected a single template param 'T'");
}

// ch05 §5.11: a full-header-form generic function may declare multiple
// type parameters, each tied to its own function-parameter position.
void test_generic_function_multiple_type_params_parses() {
    scpp::Program program = expect_parse_ok("template<typename T, typename U> void f(T a, U b) { return; }\n"
                                         "int main() { return 0; }\n");
    const scpp::Function* f_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "f") f_fn = &fn;
    }
    expect(f_fn != nullptr, "generic_function_multiple_type_params_parses: expected a Function named 'f'");
    expect(f_fn->template_params.size() == 2 && f_fn->template_params[0].name == "T" &&
               f_fn->template_params[1].name == "U",
           "generic_function_multiple_type_params_parses: expected template params [T, U]");
}

// ch05 §5.11: an abbreviated generic parameter may be a trailing pack,
// usable via a fold expression.
void test_abbreviated_generic_parameter_pack_and_fold_parse() {
    scpp::Program program = expect_parse_ok("template<typename T> concept HasGet = requires(T t) { t.get(); };\n"
                                         "int sum_two(const HasGet auto&... args) {\n"
                                         "    return (args.get() + ...);\n"
                                         "}\n"
                                         "int main() { return 0; }\n");
    const scpp::Function* sum_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "sum_two") sum_fn = &fn;
    }
    expect(sum_fn != nullptr, "abbreviated_generic_parameter_pack_and_fold_parse: expected function 'sum_two'");
    expect(sum_fn->params.size() == 1 && sum_fn->params[0].is_parameter_pack,
           "abbreviated_generic_parameter_pack_and_fold_parse: expected a single pack parameter");
    expect(!sum_fn->params[0].generic_concept.empty(),
           "abbreviated_generic_parameter_pack_and_fold_parse: pack should be generic-constrained");
    const scpp::Stmt& ret = *sum_fn->body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return && ret.expr != nullptr && ret.expr->kind == scpp::ExprKind::Fold,
           "abbreviated_generic_parameter_pack_and_fold_parse: expected a Fold return expression");
}

// ch05 §5.11: `name<Arg>(...)` -- an explicit call-site template
// argument (e.g. a "return-type-only" generic, `make<Circle>()`) --
// recorded on the Call expression's own explicit_template_args.
void test_explicit_type_template_argument_call_parses() {
    scpp::Program program = expect_parse_ok("class Circle {\n"
                                         "public:\n"
                                         "    Circle() { return; }\n"
                                         "};\n"
                                         "template<typename T> T make() {\n"
                                         "    T x{};\n"
                                         "    return x;\n"
                                         "}\n"
                                         "int main() {\n"
                                         "    Circle c = make<Circle>();\n"
                                         "    return 0;\n"
                                         "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Stmt& var_decl = *main_fn->body->statements[0];
    expect(var_decl.init != nullptr && var_decl.init->kind == scpp::ExprKind::Call,
           "explicit_type_template_argument_call_parses: expected a Call initializer");
    expect(var_decl.init->explicit_template_args.size() == 1 && var_decl.init->explicit_template_args[0].is_type &&
               is_named_type(var_decl.init->explicit_template_args[0].type, "Circle"),
           "explicit_type_template_argument_call_parses: expected explicit_template_args == [Circle]");
}

// ch05 §5.11: an explicit-template-argument call still parses as an
// ordinary multi-argument Call expression, not as a one-argument call
// accidentally terminated at the first comma.
void test_explicit_template_argument_call_with_multiple_value_args_parses() {
    scpp::Program program = expect_parse_ok("template<typename T> int pick(T x, int y) {\n"
                                         "    return y;\n"
                                         "}\n"
                                         "int main() {\n"
                                         "    int value = pick<int>(1, 42);\n"
                                         "    return value;\n"
                                         "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Stmt& var_decl = *main_fn->body->statements[0];
    expect(var_decl.init != nullptr && var_decl.init->kind == scpp::ExprKind::Call && var_decl.init->name == "pick",
           "explicit_template_argument_call_with_multiple_value_args_parses: expected a Call to 'pick'");
    expect(var_decl.init->args.size() == 2,
           "explicit_template_argument_call_with_multiple_value_args_parses: expected two value arguments");
    expect(var_decl.init->explicit_template_args.size() == 1 && var_decl.init->explicit_template_args[0].is_type &&
               is_named_type(var_decl.init->explicit_template_args[0].type, "int"),
           "explicit_template_argument_call_with_multiple_value_args_parses: expected explicit_template_args == [int]");
}

// ch05 §5.14: `name<2>(t)` -- an explicit non-type call-site template
// argument (ch05 §5.14's base-class-deduction accessor pattern,
// `get<I>`) -- recorded as a non-type (value) ExplicitTemplateArg, not a
// type one, disambiguating from the classic `a < b > c` parse.
void test_explicit_non_type_template_argument_call_parses() {
    scpp::Program program = expect_parse_ok("template<int Idx, typename... Ts> class TupleImpl;\n"
                                         "template<int Idx> class TupleImpl<Idx> {};\n"
                                         "template<int Idx, typename Head, typename... Tail>\n"
                                         "class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {\n"
                                         "public:\n"
                                         "    Head value;\n"
                                         "};\n"
                                         "template<int I, typename Head, typename... Tail>\n"
                                         "Head& get(TupleImpl<I, Head, Tail...>& t) { return t.value; }\n"
                                         "int main() {\n"
                                         "    TupleImpl<0, int, bool> t{};\n"
                                         "    int x = get<0>(t);\n"
                                         "    return 0;\n"
                                         "}\n");
    const scpp::Function* main_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "main") main_fn = &fn;
    }
    const scpp::Stmt& var_decl = *main_fn->body->statements[1];
    expect(var_decl.init != nullptr && var_decl.init->kind == scpp::ExprKind::Call && var_decl.init->name == "get",
           "explicit_non_type_template_argument_call_parses: expected a Call to 'get'");
    expect(var_decl.init->explicit_template_args.size() == 1 && !var_decl.init->explicit_template_args[0].is_type &&
               var_decl.init->explicit_template_args[0].value != nullptr,
           "explicit_non_type_template_argument_call_parses: expected a single non-type explicit_template_arg");
}

// ch05 §5.14: TupleImpl's own leading non-type parameter ("Idx") is
// legal both alone (the empty-pack base case, `TupleImpl<Idx>`) and
// followed by exactly one type parameter plus a pack (the recursive
// case, `TupleImpl<Idx, Head, Tail...>`) -- both specializations parse,
// and the recursive case's own base clause records a non-type base
// argument (the "Idx + 1" expression) alongside pack_arg_name ("Tail").
void test_variadic_specialization_with_leading_non_type_param_parses() {
    scpp::Program program = expect_parse_ok("template<int Idx, typename... Ts> class TupleImpl;\n"
                                         "template<int Idx> class TupleImpl<Idx> {};\n"
                                         "template<int Idx, typename Head, typename... Tail>\n"
                                         "class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {\n"
                                         "public:\n"
                                         "    Head value;\n"
                                         "};\n"
                                         "int main() { return 0; }\n");
    const scpp::ClassDef* base_case = nullptr;
    const scpp::ClassDef* recursive_case = nullptr;
    for (const scpp::ClassDef& c : program.classes) {
        if (c.name == "TupleImpl" && c.is_variadic_specialization) {
            if (c.template_params.size() == 1) base_case = &c;
            if (c.template_params.size() == 3) recursive_case = &c;
        }
    }
    expect(base_case != nullptr && base_case->template_params[0].is_non_type,
           "variadic_specialization_with_leading_non_type_param_parses: expected a 1-param (Idx) base case");
    expect(recursive_case != nullptr, "variadic_specialization_with_leading_non_type_param_parses: expected a "
                                       "3-param (Idx, Head, Tail) recursive case");
    expect(recursive_case->template_params[0].is_non_type && !recursive_case->template_params[1].is_non_type &&
               recursive_case->template_params[2].is_pack,
           "variadic_specialization_with_leading_non_type_param_parses: expected params [Idx(non-type), Head, "
           "Tail(pack)]");
    expect(recursive_case->base_specifiers.size() == 1 &&
               recursive_case->base_specifiers[0].base_type.name == "TupleImpl" &&
               recursive_case->base_specifiers[0].base_type.non_type_args.size() == 1 &&
               recursive_case->base_specifiers[0].base_type.non_type_args[0] != nullptr &&
               recursive_case->base_specifiers[0].pack_arg_name == "Tail",
           "variadic_specialization_with_leading_non_type_param_parses: expected base='TupleImpl', "
           "one non-type base arg, pack_arg_name='Tail'");
}

void test_phase1_ast_metadata_fields_are_storable() {
    scpp::ClassDef def;
    def.name = "Example";
    def.is_interface = true;
    scpp::BaseSpecifier base;
    base.base_type.kind = scpp::TypeKind::Named;
    base.base_type.name = "IBase";
    auto non_type_arg = std::make_shared<scpp::Expr>();
    non_type_arg->kind = scpp::ExprKind::IntegerLiteral;
    non_type_arg->int_value = 0;
    base.base_type.non_type_args.push_back(non_type_arg);
    scpp::Type pack_arg;
    pack_arg.kind = scpp::TypeKind::Named;
    pack_arg.name = "Rest";
    pack_arg.is_pack_expansion = true;
    base.base_type.template_args.push_back(pack_arg);
    base.access = scpp::AccessSpecifier::Public;
    base.is_virtual = true;
    base.kind = scpp::BaseClassKind::Interface;
    base.pack_arg_name = "Rest";
    def.base_specifiers.push_back(base);
    def.using_declarations.push_back(scpp::ClassUsingDeclaration{"IBase", "work", scpp::AccessSpecifier::Public});

    scpp::Function fn;
    fn.member_owner_class = "Example";
    fn.is_virtual = true;
    fn.is_override = true;
    fn.is_pure = true;
    fn.is_defaulted = true;

    expect(def.is_interface, "phase1_ast_metadata_fields_are_storable: expected interface flag to store");
    expect(def.base_specifiers.size() == 1 && def.base_specifiers[0].is_virtual &&
               def.base_specifiers[0].kind == scpp::BaseClassKind::Interface &&
               def.base_specifiers[0].pack_arg_name == "Rest",
           "phase1_ast_metadata_fields_are_storable: expected base specifier metadata to store");
    expect(def.base_specifiers[0].base_type.non_type_args.size() == 1 &&
               def.base_specifiers[0].base_type.non_type_args[0] != nullptr &&
               def.base_specifiers[0].base_type.template_args.size() == 1 &&
               def.base_specifiers[0].base_type.template_args[0].is_pack_expansion,
           "phase1_ast_metadata_fields_are_storable: expected base type metadata to store");
    expect(def.using_declarations.size() == 1 && def.using_declarations[0].base_name == "IBase" &&
               def.using_declarations[0].member_name == "work",
           "phase1_ast_metadata_fields_are_storable: expected class-scope using declaration to store");
    expect(fn.is_virtual && fn.is_override && fn.is_pure && fn.is_defaulted,
           "phase1_ast_metadata_fields_are_storable: expected function metadata flags to store");
}

void test_namespace_relative_qualified_generic_type_declaration_parses() {
    scpp::Program program = expect_parse_ok(
        "namespace m {\n"
        "namespace detail {\n"
        "template<typename Left, typename Right>\n"
        "class Pair {\n"
        "public:\n"
        "    Pair() { return; }\n"
        "};\n"
        "}\n"
        "int f() {\n"
        "    detail::Pair<int, bool> pair{};\n"
        "    return 0;\n"
        "}\n"
        "}\n"
        "int main() { return m::f(); }\n");
    const scpp::Function* fn = find_function_named(program, "m::f");
    expect(fn != nullptr, "namespace_relative_qualified_generic_type_declaration_parses: expected m::f");
    expect(fn->body != nullptr && fn->body->statements.size() == 2,
           "namespace_relative_qualified_generic_type_declaration_parses: expected local decl + return");
    const scpp::Stmt& decl = *fn->body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl,
           "namespace_relative_qualified_generic_type_declaration_parses: first stmt should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "m::detail::Pair",
           "namespace_relative_qualified_generic_type_declaration_parses: local type should resolve to "
           "'m::detail::Pair'");
    expect(decl.type.template_args.size() == 2 && is_named_type(decl.type.template_args[0], "int") &&
               is_named_type(decl.type.template_args[1], "bool"),
           "namespace_relative_qualified_generic_type_declaration_parses: expected <int, bool> template args");
}

void test_type_alias_declaration_parses_and_resolves() {
    scpp::Program program = expect_parse_ok(
        "using Word = unsigned long;\n"
        "using WordRef = Word&;\n"
        "alignas(Word) Word global = 41;\n"
        "Word id(WordRef value) { return value; }\n"
        "int main() {\n"
        "    Word local = global;\n"
        "    return sizeof(Word) == sizeof(unsigned long) ? id(local) - 41 : 1;\n"
        "}\n");
    expect(program.type_aliases.size() == 2, "type_alias_declaration_parses_and_resolves: expected 2 aliases");
    const scpp::TypeAliasDecl* word = find_type_alias_named(program, "Word");
    expect(word != nullptr, "type_alias_declaration_parses_and_resolves: expected alias 'Word'");
    if (word != nullptr) {
        expect(is_named_type(word->underlying_type, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: Word should alias unsigned long");
    }
    const scpp::TypeAliasDecl* word_ref = find_type_alias_named(program, "WordRef");
    expect(word_ref != nullptr, "type_alias_declaration_parses_and_resolves: expected alias 'WordRef'");
    if (word_ref != nullptr) {
        expect(word_ref->underlying_type.kind == scpp::TypeKind::Reference &&
                   word_ref->underlying_type.pointee != nullptr &&
                   is_named_type(*word_ref->underlying_type.pointee, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: WordRef should alias unsigned long&");
    }
    expect(program.globals.size() == 1, "type_alias_declaration_parses_and_resolves: expected one global");
    if (!program.globals.empty() && program.globals[0].decl != nullptr) {
        expect(is_named_type(program.globals[0].decl->type, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: global should use aliased underlying type");
        expect(program.globals[0].decl->alignment_specs.size() == 1 &&
                   program.globals[0].decl->alignment_specs[0].operand_is_type &&
                   is_named_type(program.globals[0].decl->alignment_specs[0].type, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: alignas(Word) should resolve to unsigned long");
    }
    const scpp::Function* id = find_function_named(program, "id");
    expect(id != nullptr, "type_alias_declaration_parses_and_resolves: expected function id");
    if (id != nullptr) {
        expect(is_named_type(id->return_type, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: alias return type should resolve");
        expect(id->params.size() == 1 && id->params[0].type.kind == scpp::TypeKind::Reference &&
                   id->params[0].type.pointee != nullptr && is_named_type(*id->params[0].type.pointee, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: alias parameter type should resolve");
    }
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "type_alias_declaration_parses_and_resolves: expected main");
    if (main_fn != nullptr && !main_fn->body->statements.empty()) {
        const scpp::Stmt& decl = *main_fn->body->statements[0];
        expect(decl.kind == scpp::StmtKind::VarDecl && is_named_type(decl.type, "unsigned long"),
               "type_alias_declaration_parses_and_resolves: local alias type should resolve");
    }
}

void test_exported_type_alias_inside_namespace_parses() {
    scpp::Program program = expect_parse_ok(
        "export module demo;\n"
        "namespace demo {\n"
        "    export using Word = int;\n"
        "}\n");
    expect(program.type_aliases.size() == 1, "exported_type_alias_inside_namespace_parses: expected 1 alias");
    if (program.type_aliases.size() != 1) return;
    const scpp::TypeAliasDecl& alias = program.type_aliases[0];
    expect(alias.is_exported, "exported_type_alias_inside_namespace_parses: alias should be exported");
    expect(alias.name == "demo::Word", "exported_type_alias_inside_namespace_parses: alias should be namespace-qualified");
    expect(is_named_type(alias.underlying_type, "int"),
           "exported_type_alias_inside_namespace_parses: alias should preserve underlying type");
}

void test_break_and_continue_parse_inside_loop() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    while (true) {\n"
        "        continue;\n"
        "        break;\n"
        "    }\n"
        "    return 0;\n"
        "}\n");
    const scpp::Function& main_fn = program.functions[0];
    expect(main_fn.body->statements.size() == 2, "break_and_continue_parse_inside_loop: expected while + return");
    const scpp::Stmt& while_stmt = *main_fn.body->statements[0];
    expect(while_stmt.kind == scpp::StmtKind::While, "break_and_continue_parse_inside_loop: expected While");
    expect(while_stmt.then_branch != nullptr && while_stmt.then_branch->statements.size() == 2,
           "break_and_continue_parse_inside_loop: expected 2 loop-body statements");
    expect(while_stmt.then_branch->statements[0]->kind == scpp::StmtKind::Continue,
           "break_and_continue_parse_inside_loop: first loop-body stmt should be Continue");
    expect(while_stmt.then_branch->statements[1]->kind == scpp::StmtKind::Break,
           "break_and_continue_parse_inside_loop: second loop-body stmt should be Break");
}

void test_break_outside_loop_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int main() { break; return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "break_outside_loop_is_rejected: expected a ParseError");
}

void test_continue_outside_loop_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int main() { continue; return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "continue_outside_loop_is_rejected: expected a ParseError");
}

void test_switch_statement_parses_with_cases_default_and_fallthrough() {
    scpp::Program program = expect_parse_ok(
        "enum class Color { red = 1, green = 2 };\n"
        "int main() {\n"
        "    Color color = Color::red;\n"
        "    switch (color) {\n"
        "        case Color::red:\n"
        "            return 1;\n"
        "        case Color::green:\n"
        "            [[fallthrough]];\n"
        "        default:\n"
        "            return 0;\n"
        "    }\n"
        "}\n");
    const scpp::Function& main_fn = program.functions[0];
    expect(main_fn.body->statements.size() == 2,
           "switch_statement_parses_with_cases_default_and_fallthrough: expected decl + switch");
    if (main_fn.body->statements.size() != 2) return;
    const scpp::Stmt& switch_stmt = *main_fn.body->statements[1];
    expect(switch_stmt.kind == scpp::StmtKind::Switch,
           "switch_statement_parses_with_cases_default_and_fallthrough: expected Switch");
    expect(switch_stmt.switch_cases.size() == 3,
           "switch_statement_parses_with_cases_default_and_fallthrough: expected 3 switch cases");
    if (switch_stmt.switch_cases.size() != 3) return;
    expect(switch_stmt.switch_cases[0].value != nullptr &&
               switch_stmt.switch_cases[0].value->kind == scpp::ExprKind::Identifier &&
               switch_stmt.switch_cases[0].value->name == "Color::red",
           "switch_statement_parses_with_cases_default_and_fallthrough: first case should be Color::red");
    expect(switch_stmt.switch_cases[1].statements.size() == 1 &&
               switch_stmt.switch_cases[1].statements[0]->kind == scpp::StmtKind::Fallthrough,
           "switch_statement_parses_with_cases_default_and_fallthrough: second case should end with Fallthrough");
    expect(switch_stmt.switch_cases[2].value == nullptr,
           "switch_statement_parses_with_cases_default_and_fallthrough: final case should be default");
}

// PR #414: is_explicit_switch_case_terminator now accepts a trailing
// Block whose own last statement is an explicit terminator, so a braced
// case body satisfies the "no implicit fallthrough" rule the same way a
// bare one does. The rule stays purely syntactic -- these tests pin down
// both what that newly admits and what it deliberately still rejects.
//
// Asserts on the diagnostic *text*, not merely on failure, so a case that
// starts failing for some unrelated reason (e.g. a syntax error in the
// fixture itself) cannot masquerade as the rejection under test.
void expect_switch_parse_error(std::string_view source, std::string_view needle, const std::string& case_name) {
    auto result = scpp::parse(source);
    expect(!result.has_value(), case_name + ": expected a ParseError");
    if (result.has_value()) return;
    const std::string message = result.error().what();
    expect(message.find(needle) != std::string::npos,
           case_name + ": expected diagnostic to contain '" + std::string(needle) + "', got: " + message);
}

// The tail diagnostic every "case does not end explicitly" rejection
// shares, spelled once so a wording change shows up as one edit.
const char* const kSwitchCaseTerminatorDiagnostic =
    "a non-empty switch case must end with 'break;', 'return ...;', 'continue;', or '[[fallthrough]];'";

// Recursively finds the first Switch statement in a statement tree.
// Used instead of hardcoded child indices because `for` desugars into a
// Block wrapping a While, so the switch's depth is an implementation
// detail this test should not encode.
const scpp::Stmt* find_first_switch(const scpp::Stmt& stmt) {
    if (stmt.kind == scpp::StmtKind::Switch) return &stmt;
    for (const scpp::StmtPtr& child : stmt.statements) {
        if (child == nullptr) continue;
        if (const scpp::Stmt* found = find_first_switch(*child); found != nullptr) return found;
    }
    if (stmt.then_branch != nullptr) {
        if (const scpp::Stmt* found = find_first_switch(*stmt.then_branch); found != nullptr) return found;
    }
    if (stmt.else_branch != nullptr) {
        if (const scpp::Stmt* found = find_first_switch(*stmt.else_branch); found != nullptr) return found;
    }
    for (const scpp::SwitchCase& switch_case : stmt.switch_cases) {
        for (const scpp::StmtPtr& child : switch_case.statements) {
            if (child == nullptr) continue;
            if (const scpp::Stmt* found = find_first_switch(*child); found != nullptr) return found;
        }
    }
    return nullptr;
}

void test_braced_switch_case_body_ending_in_terminator_is_accepted() {
    // One braced body per explicit terminator kind. `continue;` needs an
    // enclosing loop, so this shape doubles as the loop-context case.
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < 3; i = i + 1) {\n"
        "        switch (i) {\n"
        "            case 0: {\n"
        "                int step = 1;\n"
        "                total = total + step;\n"
        "                break;\n"
        "            }\n"
        "            case 1: {\n"
        "                int step = 10;\n"
        "                continue;\n"
        "            }\n"
        "            default: {\n"
        "                return total;\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "    return total;\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "braced_switch_case_body_ending_in_terminator_is_accepted: expected main");
    if (main_fn == nullptr || main_fn->body == nullptr) return;
    // Each case body should be a single Block statement -- i.e. the braces
    // really were parsed as a block, not silently flattened into the
    // shared switch scope (the scoping half of this change).
    const scpp::Stmt* switch_stmt = find_first_switch(*main_fn->body);
    expect(switch_stmt != nullptr,
           "braced_switch_case_body_ending_in_terminator_is_accepted: expected a Switch");
    if (switch_stmt == nullptr) return;
    expect(switch_stmt->switch_cases.size() == 3,
           "braced_switch_case_body_ending_in_terminator_is_accepted: expected 3 cases");
    if (switch_stmt->switch_cases.size() != 3) return;
    for (std::size_t i = 0; i < switch_stmt->switch_cases.size(); i++) {
        expect(switch_stmt->switch_cases[i].statements.size() == 1 &&
                   switch_stmt->switch_cases[i].statements[0]->kind == scpp::StmtKind::Block,
               "braced_switch_case_body_ending_in_terminator_is_accepted: case " + std::to_string(i) +
                   " should be a single Block statement");
    }
    // The two sibling `step` locals prove braces give each case its own
    // scope -- this source would be a redeclaration without them.
}

void test_nested_braced_switch_case_bodies_compose() {
    // block_body_terminates_switch_case recurses, so arbitrarily nested
    // blocks terminate the case as long as the innermost tail does.
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    switch (1) {\n"
        "        case 1: {\n"
        "            {\n"
        "                {\n"
        "                    return 0;\n"
        "                }\n"
        "            }\n"
        "        }\n"
        "        default:\n"
        "            return 1;\n"
        "    }\n"
        "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "nested_braced_switch_case_bodies_compose: expected main");
}

void test_empty_braced_switch_case_body_is_rejected() {
    // `{ }` terminates nothing, so it is still an implicit fallthrough.
    // block_body_terminates_switch_case's `!stmt.statements.empty()` guard
    // is what keeps this an error.
    expect_switch_parse_error("int main() {\n"
                              "    switch (1) {\n"
                              "        case 1: {\n"
                              "        }\n"
                              "        default:\n"
                              "            return 1;\n"
                              "    }\n"
                              "}\n",
                              kSwitchCaseTerminatorDiagnostic, "empty_braced_switch_case_body_is_rejected");
}

void test_braced_switch_case_body_without_terminator_is_rejected() {
    // Braces alone do not satisfy the rule; the block's own last statement
    // still has to be a terminator.
    expect_switch_parse_error("int main() {\n"
                              "    switch (1) {\n"
                              "        case 1: {\n"
                              "            int value = 0;\n"
                              "            value = value + 1;\n"
                              "        }\n"
                              "        default:\n"
                              "            return 1;\n"
                              "    }\n"
                              "}\n",
                              kSwitchCaseTerminatorDiagnostic,
                              "braced_switch_case_body_without_terminator_is_rejected");
}

void test_braced_switch_case_body_ending_in_if_else_is_rejected() {
    // Deliberately still rejected even though both branches return: the
    // check is syntactic and does not do the flow analysis that would be
    // needed to prove an if/else always terminates.
    expect_switch_parse_error("int main() {\n"
                              "    switch (1) {\n"
                              "        case 1: {\n"
                              "            if (1 == 1) {\n"
                              "                return 0;\n"
                              "            } else {\n"
                              "                return 2;\n"
                              "            }\n"
                              "        }\n"
                              "        default:\n"
                              "            return 1;\n"
                              "    }\n"
                              "}\n",
                              kSwitchCaseTerminatorDiagnostic,
                              "braced_switch_case_body_ending_in_if_else_is_rejected");
}

void test_fallthrough_nested_in_braced_switch_case_body_is_rejected() {
    // `[[fallthrough]];` is deliberately absent from
    // block_body_terminates_switch_case: it describes control leaving the
    // *case*, not the inner block. reject_nested_fallthrough runs first
    // and is what produces this diagnostic, so the assertion pins that
    // specific wording rather than the generic terminator one -- i.e.
    // `case X: { [[fallthrough]]; }` is handled deliberately.
    expect_switch_parse_error("int main() {\n"
                              "    switch (1) {\n"
                              "        case 1: {\n"
                              "            [[fallthrough]];\n"
                              "        }\n"
                              "        default:\n"
                              "            return 1;\n"
                              "    }\n"
                              "}\n",
                              "'[[fallthrough]];' is only valid as the final top-level statement of a switch case",
                              "fallthrough_nested_in_braced_switch_case_body_is_rejected");
}

void test_top_level_fallthrough_after_braced_body_is_still_accepted() {
    // The pre-existing top-level `[[fallthrough]];` positioning rule is
    // unaffected by the block change: it is still accepted as a case's own
    // final top-level statement, even when preceded by a braced body.
    scpp::Program program = expect_parse_ok("int main() {\n"
                                            "    switch (1) {\n"
                                            "        case 1: {\n"
                                            "            int value = 0;\n"
                                            "            value = value + 1;\n"
                                            "        }\n"
                                            "        [[fallthrough]];\n"
                                            "        default:\n"
                                            "            return 1;\n"
                                            "    }\n"
                                            "}\n");
    const scpp::Function* main_fn = find_function_named(program, "main");
    expect(main_fn != nullptr, "top_level_fallthrough_after_braced_body_is_still_accepted: expected main");
}

void test_bare_switch_case_without_terminator_is_still_rejected() {
    // Regression guard on the *unchanged* half of the rule: a bare
    // (unbraced) case body that does not end in a terminator must still
    // produce the same diagnostic it always did.
    expect_switch_parse_error("int main() {\n"
                              "    switch (1) {\n"
                              "        case 1:\n"
                              "            return 0;\n"
                              "        default:\n"
                              "            int value = 0;\n"
                              "    }\n"
                              "}\n",
                              kSwitchCaseTerminatorDiagnostic,
                              "bare_switch_case_without_terminator_is_still_rejected");
}

void test_namespaced_enum_case_label_resolves_to_qualified_enumerator() {
    scpp::Program program = expect_parse_ok(
        "namespace calc {\n"
        "enum class Color { red = 1, green = 2 };\n"
        "int pick(Color color) {\n"
        "    switch (color) {\n"
        "        case Color::red:\n"
        "            return 1;\n"
        "        case Color::green:\n"
        "            return 0;\n"
        "        default:\n"
        "            return 2;\n"
        "    }\n"
        "}\n"
        "}\n");
    const scpp::Function& pick_fn = program.functions[0];
    expect(pick_fn.body->statements.size() == 1,
           "namespaced_enum_case_label_resolves_to_qualified_enumerator: expected a single switch statement");
    if (pick_fn.body->statements.size() != 1) return;
    const scpp::Stmt& switch_stmt = *pick_fn.body->statements[0];
    expect(switch_stmt.kind == scpp::StmtKind::Switch,
           "namespaced_enum_case_label_resolves_to_qualified_enumerator: expected Switch");
    expect(switch_stmt.switch_cases.size() == 3,
           "namespaced_enum_case_label_resolves_to_qualified_enumerator: expected 3 switch cases");
    if (switch_stmt.switch_cases.size() < 2) return;
    expect(switch_stmt.switch_cases[0].value != nullptr &&
               switch_stmt.switch_cases[0].value->kind == scpp::ExprKind::Identifier &&
               switch_stmt.switch_cases[0].value->name == "calc::Color::red",
           "namespaced_enum_case_label_resolves_to_qualified_enumerator: first case should resolve to calc::Color::red");
    expect(switch_stmt.switch_cases[1].value != nullptr &&
               switch_stmt.switch_cases[1].value->kind == scpp::ExprKind::Identifier &&
               switch_stmt.switch_cases[1].value->name == "calc::Color::green",
           "namespaced_enum_case_label_resolves_to_qualified_enumerator: second case should resolve to calc::Color::green");
}

void test_break_parses_inside_switch() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    switch (1) {\n"
        "        case 1:\n"
        "            break;\n"
        "        default:\n"
        "            return 0;\n"
        "    }\n"
        "    return 1;\n"
        "}\n");
    const scpp::Stmt& switch_stmt = *program.functions[0].body->statements[0];
    expect(switch_stmt.kind == scpp::StmtKind::Switch, "break_parses_inside_switch: expected Switch");
    expect(switch_stmt.switch_cases.size() == 2, "break_parses_inside_switch: expected 2 cases");
    if (switch_stmt.switch_cases.size() != 2) return;
    expect(switch_stmt.switch_cases[0].statements.size() == 1 &&
               switch_stmt.switch_cases[0].statements[0]->kind == scpp::StmtKind::Break,
           "break_parses_inside_switch: first case should contain Break");
}

void test_fallthrough_outside_switch_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("int main() { [[fallthrough]]; return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "fallthrough_outside_switch_is_rejected: expected a ParseError");
}

void test_fallthrough_must_be_last_in_case() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "int main() {\n"
            "    switch (1) {\n"
            "        case 1:\n"
            "            [[fallthrough]];\n"
            "            return 1;\n"
            "        default:\n"
            "            return 0;\n"
            "    }\n"
            "}\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "fallthrough_must_be_last_in_case: expected a ParseError");
}

void test_switch_case_requires_explicit_terminator() {
    bool threw = false;
    if (auto _r = scpp::parse(
            "int main() {\n"
            "    switch (1) {\n"
            "        case 1:\n"
            "            int x = 1;\n"
            "        default:\n"
            "            return 0;\n"
            "    }\n"
            "}\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "switch_case_requires_explicit_terminator: expected a ParseError");
}

void test_grouped_switch_case_labels_parse_as_empty_cases() {
    scpp::Program program = expect_parse_ok(
        "int main() {\n"
        "    switch (2) {\n"
        "        case 1:\n"
        "        case 2:\n"
        "            return 0;\n"
        "        default:\n"
        "            return 1;\n"
        "    }\n"
        "}\n");
    const scpp::Stmt& switch_stmt = *program.functions[0].body->statements[0];
    expect(switch_stmt.kind == scpp::StmtKind::Switch,
           "grouped_switch_case_labels_parse_as_empty_cases: expected Switch");
    expect(switch_stmt.switch_cases.size() == 3,
           "grouped_switch_case_labels_parse_as_empty_cases: expected 3 switch cases");
    if (switch_stmt.switch_cases.size() != 3) return;
    expect(switch_stmt.switch_cases[0].statements.empty(),
           "grouped_switch_case_labels_parse_as_empty_cases: first grouped case should be empty");
    expect(switch_stmt.switch_cases[1].statements.size() == 1 &&
               switch_stmt.switch_cases[1].statements[0]->kind == scpp::StmtKind::Return,
           "grouped_switch_case_labels_parse_as_empty_cases: second grouped case should own the shared body");
}

void test_conditional_expression_parses() {
    scpp::Program program = expect_parse_ok("int main() { return true ? 1 : 2; }\n");
    const scpp::Function& main_fn = program.functions[0];
    expect(main_fn.body->statements.size() == 1, "conditional_expression_parses: expected 1 statement");
    const scpp::Stmt& ret = *main_fn.body->statements[0];
    expect(ret.kind == scpp::StmtKind::Return, "conditional_expression_parses: expected Return");
    expect(ret.expr != nullptr && ret.expr->kind == scpp::ExprKind::Conditional,
           "conditional_expression_parses: return expr should be Conditional");
    expect(ret.expr->lhs != nullptr && ret.expr->rhs != nullptr && ret.expr->third != nullptr,
           "conditional_expression_parses: expected condition, then, and else arms");
}

void test_enum_class_declaration_parses() {
    scpp::Program program = expect_parse_ok(
        "enum class Color { red, green = 4, blue };\n"
        "int main() { return 0; }\n");
    expect(program.enums.size() == 1, "enum_class_declaration_parses: expected 1 enum");
    if (program.enums.size() != 1) return;
    const scpp::EnumDef& def = program.enums[0];
    expect(def.name == "Color", "enum_class_declaration_parses: expected enum name Color");
    expect(is_named_type(def.underlying_type, "int"),
           "enum_class_declaration_parses: expected default underlying type int");
    expect(def.variants.size() == 3, "enum_class_declaration_parses: expected 3 variants");
    if (def.variants.size() != 3) return;
    expect(def.variants[0].name == "Color::red" && def.variants[0].value == 0,
           "enum_class_declaration_parses: expected Color::red = 0");
    expect(def.variants[1].name == "Color::green" && def.variants[1].value == 4,
           "enum_class_declaration_parses: expected Color::green = 4");
    expect(def.variants[2].name == "Color::blue" && def.variants[2].value == 5,
           "enum_class_declaration_parses: expected Color::blue = 5");
}

void test_enum_class_underlying_type_parses() {
    scpp::Program program = expect_parse_ok(
        "enum class Small : uint8_t { a = 1, b = 3 };\n"
        "int main() { return 0; }\n");
    expect(program.enums.size() == 1, "enum_class_underlying_type_parses: expected 1 enum");
    if (program.enums.empty()) return;
    const scpp::EnumDef& def = program.enums[0];
    expect(def.name == "Small", "enum_class_underlying_type_parses: expected enum name Small");
    expect(is_named_type(def.underlying_type, "uint8_t"),
           "enum_class_underlying_type_parses: expected underlying type uint8_t");
    expect(def.variants.size() == 2, "enum_class_underlying_type_parses: expected 2 variants");
    if (def.variants.size() != 2) return;
    expect(def.variants[0].name == "Small::a" && def.variants[0].value == 1,
           "enum_class_underlying_type_parses: expected Small::a = 1");
    expect(def.variants[1].name == "Small::b" && def.variants[1].value == 3,
           "enum_class_underlying_type_parses: expected Small::b = 3");
}

void test_old_style_enum_is_rejected() {
    bool threw = false;
    if (auto _r = scpp::parse("enum Color { red, green };\nint main() { return 0; }\n"); !_r.has_value()) {
        threw = true;
    }
    expect(threw, "old_style_enum_is_rejected: expected a ParseError");
}

// scpp::parse's public API surface returns std::expected<Program, ParseError>
// rather than throwing ParseError (parser.cppm has no exceptions of its own,
// a prerequisite for eventually self-hosting this file) -- these two tests
// exercise that contract directly, independent of any test-file-local
// helper (expect_parse_ok/parse_with_std_imports) that wraps it.
void test_parse_returns_engaged_expected_on_success() {
    std::expected<scpp::Program, scpp::ParseError> result = scpp::parse("int main() { return 0; }\n");
    expect(result.has_value(), "parse_returns_engaged_expected_on_success: expected has_value() to be true");
    if (!result.has_value()) return;
    expect(result->functions.size() == 1,
           "parse_returns_engaged_expected_on_success: expected 1 function via operator->");
    expect(result.value().functions[0].name == "main",
           "parse_returns_engaged_expected_on_success: expected function named 'main' via .value()");
}

void test_parse_returns_disengaged_expected_on_failure_without_throwing() {
    // No try/catch here at all -- if scpp::parse still threw instead of
    // returning std::expected, this call itself would already have
    // aborted the test binary before reaching any of the expect() calls
    // below, since nothing in this function catches exceptions.
    std::expected<scpp::Program, scpp::ParseError> result = scpp::parse("int main() { return 0\n");
    expect(!result.has_value(),
           "parse_returns_disengaged_expected_on_failure_without_throwing: expected has_value() to be false");
    if (result.has_value()) return;
    const scpp::ParseError& error = result.error();
    expect(error.loc.is_known(),
           "parse_returns_disengaged_expected_on_failure_without_throwing: expected a known error location");
    expect(error.line > 0 && error.column > 0,
           "parse_returns_disengaged_expected_on_failure_without_throwing: expected positive line/column");
    expect(std::string(error.what()).size() > 0,
           "parse_returns_disengaged_expected_on_failure_without_throwing: expected a non-empty diagnostic message");
}

// ---------------------------------------------------------------------------
// Structural guard for the AST node cloners.
//
// Every clone of an `Expr` or a `Stmt` in the compiler funnels through exactly
// one enumeration -- `scpp::assign_expr_fields` / `scpp::assign_stmt_fields` in
// `src/compiler/ast.cppm`. That is a real improvement over the six `Expr` and
// four `Stmt` hand-written cloners that used to exist and had silently drifted
// apart, but it is still an enumeration a human has to keep in sync: scpp has
// no reflection, and the node types cannot use a defaulted copy constructor
// because a value-semantic owning pointer (`deep_ptr<T>`) is not expressible in
// the language (`std::make_unique<T>(*other.ptr)` copies a borrow by value,
// which ch05 forbids), so `ast.cppm` -- which must stay self-hostable -- has to
// spell the fields out.
//
// The guards below make the omission *loud* instead of silent. They bind every
// member of each node type by name; adding, removing or reordering a field
// makes the corresponding binding fail to compile with a message naming the
// type and both counts, e.g.
//
//     error: type 'scpp::Expr' binds to 30 elements, but only 29 names were
//     provided
//
// If you land here after adding a field:
//   1. add it to `assign_expr_fields` / `assign_stmt_fields` in ast.cppm,
//   2. add its name to the binding below, and
//   3. give it a distinctive value in the round-trip test underneath and assert
//      that the clone carries it.
// Step 3 is what actually proves the field is copied; the binding exists to
// stop step 1 from being forgotten, which is how `Expr::resolved_local` was
// once dropped by a subset of the cloners.
// ---------------------------------------------------------------------------

void test_expr_field_count_is_guarded() {
    const scpp::Expr probe{};
    const auto& [kind, resolved_local, loc, int_value, float_value, bool_value, name,
                 explicit_global_qualification, binary_op, lhs, rhs, third, fold_ellipsis_on_left,
                 unary_op, args, explicit_template_args, type, sizeof_operand_is_type, has_paren_init,
                 destroy_through_pointer, through_arrow, implicit_arrow_deref, implicit_arrow_chain_safe,
                 lambda_captures, lambda_blanket_mode, lambda_params, has_lambda_explicit_return_type,
                 lambda_is_mutable, lambda_body] = probe;
    expect(kind == scpp::ExprKind::IntegerLiteral && resolved_local == 0 && loc.line == 0 &&
               int_value == 0 && float_value == 0.0 && !bool_value && name.empty() &&
               !explicit_global_qualification && lhs == nullptr && rhs == nullptr && third == nullptr &&
               !fold_ellipsis_on_left && args.empty() && explicit_template_args.empty() &&
               type.kind == scpp::TypeKind::Named && type.name.empty() && !sizeof_operand_is_type && !has_paren_init &&
               !destroy_through_pointer && !through_arrow && !implicit_arrow_deref &&
               !implicit_arrow_chain_safe && lambda_captures.empty() &&
               lambda_blanket_mode == scpp::LambdaCaptureMode::None && lambda_params.empty() &&
               !has_lambda_explicit_return_type && !lambda_is_mutable && lambda_body == nullptr &&
               binary_op == scpp::BinaryOp::Add && unary_op == scpp::UnaryOp::Neg,
           "a default-constructed Expr value-initializes every field");
}

void test_stmt_field_count_is_guarded() {
    const scpp::Stmt probe{};
    const auto& [kind, loc, type, var_name, declared_local, init, alignment_specs, resolved_alignment,
                 is_const, is_constexpr, is_static_local, has_ctor_args, ctor_args, expr, condition,
                 if_mode, then_branch, else_branch, switch_cases, statements, is_unsafe] = probe;
    expect(kind == scpp::StmtKind::VarDecl && loc.line == 0 && type.kind == scpp::TypeKind::Named &&
               var_name.empty() && declared_local == 0 && init == nullptr && alignment_specs.empty() &&
               resolved_alignment == 0 && !is_const && !is_constexpr && !is_static_local &&
               !has_ctor_args && ctor_args.empty() && expr == nullptr && condition == nullptr &&
               if_mode == scpp::IfMode::Runtime && then_branch == nullptr && else_branch == nullptr &&
               switch_cases.empty() && statements.empty() && !is_unsafe,
           "a default-constructed Stmt value-initializes every field");
}

void test_lambda_capture_field_count_is_guarded() {
    const scpp::LambdaCapture probe{};
    const auto& [name, by_reference, init, resolved_local] = probe;
    expect(name.empty() && !by_reference && init == nullptr && resolved_local == 0,
           "a default-constructed LambdaCapture value-initializes every field");
}

void test_switch_case_field_count_is_guarded() {
    const scpp::SwitchCase probe{};
    const auto& [loc, value, statements] = probe;
    expect(loc.line == 0 && value == nullptr && statements.empty(),
           "a default-constructed SwitchCase value-initializes every field");
}

void test_explicit_template_arg_field_count_is_guarded() {
    const scpp::ExplicitTemplateArg probe{};
    const auto& [is_type, type, value] = probe;
    expect(is_type && type.kind == scpp::TypeKind::Named && value == nullptr,
           "a default-constructed ExplicitTemplateArg value-initializes every field");
}

void test_param_field_count_is_guarded() {
    const scpp::Param probe{};
    const auto& [type, name, resolved_local, lifetime, default_expr, generic_concept,
                 require_thread_movable, require_thread_shareable, is_parameter_pack] = probe;
    expect(type.kind == scpp::TypeKind::Named && name.empty() && resolved_local == 0 &&
               !lifetime.present() && default_expr == nullptr && generic_concept.empty() &&
               !require_thread_movable && !require_thread_shareable && !is_parameter_pack,
           "a default-constructed Param value-initializes every field");
}

// Builds an `Expr` in which *every* field holds a value distinguishable from
// the default, so a clone that drops any one of them is detectable.
scpp::Expr make_fully_populated_expr() {
    scpp::Expr e{};
    e.kind = scpp::ExprKind::Binary;
    e.resolved_local = 41;
    e.loc = scpp::SourceLocation{7, 13};
    e.int_value = 1234;
    e.float_value = 2.5;
    e.bool_value = true;
    e.name = "populated";
    e.explicit_global_qualification = true;
    e.binary_op = scpp::BinaryOp::Mul;
    e.lhs = std::make_unique<scpp::Expr>();
    e.lhs->kind = scpp::ExprKind::Identifier;
    e.lhs->name = "lhs_child";
    e.lhs->resolved_local = 51;
    e.rhs = std::make_unique<scpp::Expr>();
    e.rhs->kind = scpp::ExprKind::Identifier;
    e.rhs->name = "rhs_child";
    e.rhs->resolved_local = 52;
    e.third = std::make_unique<scpp::Expr>();
    e.third->kind = scpp::ExprKind::Identifier;
    e.third->name = "third_child";
    e.third->resolved_local = 53;
    e.fold_ellipsis_on_left = true;
    e.unary_op = scpp::UnaryOp::Not;
    auto arg = std::make_unique<scpp::Expr>();
    arg->kind = scpp::ExprKind::Identifier;
    arg->name = "arg0";
    arg->resolved_local = 54;
    e.args.push_back(std::move(arg));
    scpp::ExplicitTemplateArg targ{};
    targ.is_type = false;
    targ.type.kind = scpp::TypeKind::Pointer;
    auto targ_value = std::make_unique<scpp::Expr>();
    targ_value->kind = scpp::ExprKind::Identifier;
    targ_value->name = "targ_value";
    targ_value->resolved_local = 55;
    targ.value = std::shared_ptr<scpp::Expr>(targ_value.release());
    e.explicit_template_args.push_back(std::move(targ));
    e.type.name = "populated_type";
    e.sizeof_operand_is_type = true;
    e.has_paren_init = true;
    e.destroy_through_pointer = true;
    e.through_arrow = true;
    e.implicit_arrow_deref = true;
    e.implicit_arrow_chain_safe = true;
    scpp::LambdaCapture capture{};
    capture.name = "cap";
    capture.by_reference = true;
    capture.resolved_local = 56;
    capture.init = std::make_unique<scpp::Expr>();
    capture.init->kind = scpp::ExprKind::Identifier;
    capture.init->name = "cap_init";
    capture.init->resolved_local = 57;
    e.lambda_captures.push_back(std::move(capture));
    e.lambda_blanket_mode = scpp::LambdaCaptureMode::ByReference;
    scpp::Param param{};
    param.name = "p0";
    param.type.name = "param_type";
    param.resolved_local = 58;
    param.lifetime.name = "a";
    param.generic_concept = "Concept";
    param.require_thread_movable = true;
    param.require_thread_shareable = true;
    param.is_parameter_pack = true;
    auto param_default = std::make_unique<scpp::Expr>();
    param_default->kind = scpp::ExprKind::Identifier;
    param_default->name = "param_default";
    param_default->resolved_local = 59;
    param.default_expr = std::shared_ptr<scpp::Expr>(param_default.release());
    e.lambda_params.push_back(std::move(param));
    e.has_lambda_explicit_return_type = true;
    e.lambda_is_mutable = true;
    e.lambda_body = std::make_unique<scpp::Stmt>();
    e.lambda_body->kind = scpp::StmtKind::Block;
    e.lambda_body->var_name = "lambda_body";
    e.lambda_body->declared_local = 60;
    return e;
}

// Builds a `Stmt` in which every field holds a non-default value.
scpp::Stmt make_fully_populated_stmt() {
    scpp::Stmt s{};
    s.kind = scpp::StmtKind::If;
    s.loc = scpp::SourceLocation{11, 3};
    s.type.name = "stmt_type";
    s.var_name = "populated_stmt";
    s.declared_local = 71;
    s.init = std::make_unique<scpp::Expr>();
    s.init->kind = scpp::ExprKind::Identifier;
    s.init->name = "init_child";
    s.init->resolved_local = 72;
    scpp::AlignmentSpecifier align{};
    align.operand_is_type = true;
    align.type.name = "align_type";
    s.alignment_specs.push_back(std::move(align));
    s.resolved_alignment = 32;
    s.is_const = true;
    s.is_constexpr = true;
    s.is_static_local = true;
    s.has_ctor_args = true;
    auto ctor_arg = std::make_unique<scpp::Expr>();
    ctor_arg->kind = scpp::ExprKind::Identifier;
    ctor_arg->name = "ctor_arg0";
    ctor_arg->resolved_local = 73;
    s.ctor_args.push_back(std::move(ctor_arg));
    s.expr = std::make_unique<scpp::Expr>();
    s.expr->kind = scpp::ExprKind::Identifier;
    s.expr->name = "expr_child";
    s.expr->resolved_local = 74;
    s.condition = std::make_unique<scpp::Expr>();
    s.condition->kind = scpp::ExprKind::Identifier;
    s.condition->name = "condition_child";
    s.condition->resolved_local = 75;
    s.if_mode = scpp::IfMode::ConstevalTrue;
    s.then_branch = std::make_unique<scpp::Stmt>();
    s.then_branch->kind = scpp::StmtKind::Block;
    s.then_branch->var_name = "then_child";
    s.then_branch->declared_local = 76;
    s.else_branch = std::make_unique<scpp::Stmt>();
    s.else_branch->kind = scpp::StmtKind::Block;
    s.else_branch->var_name = "else_child";
    s.else_branch->declared_local = 77;
    scpp::SwitchCase switch_case{};
    switch_case.loc = scpp::SourceLocation{19, 5};
    switch_case.value = std::make_unique<scpp::Expr>();
    switch_case.value->kind = scpp::ExprKind::IntegerLiteral;
    switch_case.value->int_value = 99;
    auto case_stmt = std::make_unique<scpp::Stmt>();
    case_stmt->kind = scpp::StmtKind::Break;
    case_stmt->var_name = "case_stmt";
    case_stmt->declared_local = 78;
    switch_case.statements.push_back(std::move(case_stmt));
    s.switch_cases.push_back(std::move(switch_case));
    auto child = std::make_unique<scpp::Stmt>();
    child->kind = scpp::StmtKind::ExprStmt;
    child->var_name = "block_child";
    child->declared_local = 79;
    s.statements.push_back(std::move(child));
    s.is_unsafe = true;
    return s;
}

// The binding here is deliberately the same shape as the guard above: a new
// `Expr` field forces this assertion list to be revisited, so a field that is
// added to the node but forgotten in `assign_expr_fields` cannot slip through.
void expect_expr_round_trip(const scpp::Expr& clone, std::string_view label) {
    const auto& [kind, resolved_local, loc, int_value, float_value, bool_value, name,
                 explicit_global_qualification, binary_op, lhs, rhs, third, fold_ellipsis_on_left,
                 unary_op, args, explicit_template_args, type, sizeof_operand_is_type, has_paren_init,
                 destroy_through_pointer, through_arrow, implicit_arrow_deref, implicit_arrow_chain_safe,
                 lambda_captures, lambda_blanket_mode, lambda_params, has_lambda_explicit_return_type,
                 lambda_is_mutable, lambda_body] = clone;
    expect(kind == scpp::ExprKind::Binary, std::string{label} + ": kind survives");
    expect(resolved_local == 41, std::string{label} + ": resolved_local survives");
    expect(loc.line == 7 && loc.column == 13, std::string{label} + ": loc survives");
    expect(int_value == 1234, std::string{label} + ": int_value survives");
    expect(float_value == 2.5, std::string{label} + ": float_value survives");
    expect(bool_value, std::string{label} + ": bool_value survives");
    expect(name == "populated", std::string{label} + ": name survives");
    expect(explicit_global_qualification, std::string{label} + ": explicit_global_qualification survives");
    expect(binary_op == scpp::BinaryOp::Mul, std::string{label} + ": binary_op survives");
    expect(lhs != nullptr && lhs->name == "lhs_child" && lhs->resolved_local == 51,
           std::string{label} + ": lhs survives with its resolution");
    expect(rhs != nullptr && rhs->name == "rhs_child" && rhs->resolved_local == 52,
           std::string{label} + ": rhs survives with its resolution");
    expect(third != nullptr && third->name == "third_child" && third->resolved_local == 53,
           std::string{label} + ": third survives with its resolution");
    expect(fold_ellipsis_on_left, std::string{label} + ": fold_ellipsis_on_left survives");
    expect(unary_op == scpp::UnaryOp::Not, std::string{label} + ": unary_op survives");
    expect(args.size() == 1 && args[0] != nullptr && args[0]->name == "arg0" && args[0]->resolved_local == 54,
           std::string{label} + ": args survive with their resolution");
    expect(explicit_template_args.size() == 1 && explicit_template_args[0].value != nullptr &&
               explicit_template_args[0].value->name == "targ_value" &&
               explicit_template_args[0].value->resolved_local == 55 &&
               explicit_template_args[0].type.kind == scpp::TypeKind::Pointer,
           std::string{label} + ": explicit_template_args survive with their resolution");
    expect(type.name == "populated_type", std::string{label} + ": type survives");
    expect(sizeof_operand_is_type, std::string{label} + ": sizeof_operand_is_type survives");
    expect(has_paren_init, std::string{label} + ": has_paren_init survives");
    expect(destroy_through_pointer, std::string{label} + ": destroy_through_pointer survives");
    expect(through_arrow, std::string{label} + ": through_arrow survives");
    expect(implicit_arrow_deref, std::string{label} + ": implicit_arrow_deref survives");
    expect(implicit_arrow_chain_safe, std::string{label} + ": implicit_arrow_chain_safe survives");
    expect(lambda_captures.size() == 1 && lambda_captures[0].name == "cap" &&
               lambda_captures[0].by_reference && lambda_captures[0].resolved_local == 56 &&
               lambda_captures[0].init != nullptr && lambda_captures[0].init->name == "cap_init" &&
               lambda_captures[0].init->resolved_local == 57,
           std::string{label} + ": lambda_captures survive with their resolution");
    expect(lambda_blanket_mode == scpp::LambdaCaptureMode::ByReference,
           std::string{label} + ": lambda_blanket_mode survives");
    expect(lambda_params.size() == 1 && lambda_params[0].name == "p0" && lambda_params[0].resolved_local == 58 &&
               lambda_params[0].lifetime.name == "a" && lambda_params[0].type.name == "param_type" &&
               lambda_params[0].generic_concept == "Concept" &&
               lambda_params[0].require_thread_movable && lambda_params[0].require_thread_shareable &&
               lambda_params[0].is_parameter_pack && lambda_params[0].default_expr != nullptr &&
               lambda_params[0].default_expr->name == "param_default" &&
               lambda_params[0].default_expr->resolved_local == 59,
           std::string{label} + ": lambda_params survive with their resolution");
    expect(has_lambda_explicit_return_type, std::string{label} + ": has_lambda_explicit_return_type survives");
    expect(lambda_is_mutable, std::string{label} + ": lambda_is_mutable survives");
    expect(lambda_body != nullptr && lambda_body->var_name == "lambda_body" && lambda_body->declared_local == 60,
           std::string{label} + ": lambda_body survives with its resolution");
}

void expect_stmt_round_trip(const scpp::Stmt& clone, std::string_view label) {
    const auto& [kind, loc, type, var_name, declared_local, init, alignment_specs, resolved_alignment,
                 is_const, is_constexpr, is_static_local, has_ctor_args, ctor_args, expr, condition,
                 if_mode, then_branch, else_branch, switch_cases, statements, is_unsafe] = clone;
    expect(kind == scpp::StmtKind::If, std::string{label} + ": kind survives");
    expect(loc.line == 11 && loc.column == 3, std::string{label} + ": loc survives");
    expect(type.name == "stmt_type", std::string{label} + ": type survives");
    expect(var_name == "populated_stmt", std::string{label} + ": var_name survives");
    expect(declared_local == 71, std::string{label} + ": declared_local survives");
    expect(init != nullptr && init->name == "init_child" && init->resolved_local == 72,
           std::string{label} + ": init survives with its resolution");
    expect(alignment_specs.size() == 1 && alignment_specs[0].operand_is_type &&
               alignment_specs[0].type.name == "align_type",
           std::string{label} + ": alignment_specs survive");
    expect(resolved_alignment == 32, std::string{label} + ": resolved_alignment survives");
    expect(is_const, std::string{label} + ": is_const survives");
    expect(is_constexpr, std::string{label} + ": is_constexpr survives");
    expect(is_static_local, std::string{label} + ": is_static_local survives");
    expect(has_ctor_args, std::string{label} + ": has_ctor_args survives");
    expect(ctor_args.size() == 1 && ctor_args[0] != nullptr && ctor_args[0]->name == "ctor_arg0" &&
               ctor_args[0]->resolved_local == 73,
           std::string{label} + ": ctor_args survive with their resolution");
    expect(expr != nullptr && expr->name == "expr_child" && expr->resolved_local == 74,
           std::string{label} + ": expr survives with its resolution");
    expect(condition != nullptr && condition->name == "condition_child" && condition->resolved_local == 75,
           std::string{label} + ": condition survives with its resolution");
    expect(if_mode == scpp::IfMode::ConstevalTrue, std::string{label} + ": if_mode survives");
    expect(then_branch != nullptr && then_branch->var_name == "then_child" && then_branch->declared_local == 76,
           std::string{label} + ": then_branch survives with its resolution");
    expect(else_branch != nullptr && else_branch->var_name == "else_child" && else_branch->declared_local == 77,
           std::string{label} + ": else_branch survives with its resolution");
    expect(switch_cases.size() == 1 && switch_cases[0].loc.line == 19 && switch_cases[0].value != nullptr &&
               switch_cases[0].value->int_value == 99 && switch_cases[0].statements.size() == 1 &&
               switch_cases[0].statements[0] != nullptr &&
               switch_cases[0].statements[0]->var_name == "case_stmt" &&
               switch_cases[0].statements[0]->declared_local == 78,
           std::string{label} + ": switch_cases survive with their bodies");
    expect(statements.size() == 1 && statements[0] != nullptr && statements[0]->var_name == "block_child" &&
               statements[0]->declared_local == 79,
           std::string{label} + ": statements survive with their resolution");
    expect(is_unsafe, std::string{label} + ": is_unsafe survives");
}

// `deep_clone_expr`, the copy constructor and copy assignment are three
// entry points onto the *same* enumeration -- assert all three, because
// the drift that used to exist was precisely between such entry points.
void test_expr_clone_preserves_every_field() {
    const scpp::Expr original = make_fully_populated_expr();

    scpp::ExprPtr deep = scpp::deep_clone_expr(original);
    expect(deep != nullptr, "deep_clone_expr returns a node");
    if (deep != nullptr) expect_expr_round_trip(*deep, "deep_clone_expr");

    const scpp::Expr copy_constructed{original};
    expect_expr_round_trip(copy_constructed, "Expr copy constructor");

    scpp::Expr copy_assigned{};
    copy_assigned = original;
    expect_expr_round_trip(copy_assigned, "Expr copy assignment");

    // Deep, not shared: mutating the clone must not disturb the original.
    if (deep != nullptr && deep->lhs != nullptr) deep->lhs->name = "mutated";
    expect(original.lhs != nullptr && original.lhs->name == "lhs_child",
           "deep_clone_expr does not share children with the original");
}

void test_stmt_clone_preserves_every_field() {
    const scpp::Stmt original = make_fully_populated_stmt();

    scpp::StmtPtr deep = scpp::deep_clone_stmt(original);
    expect(deep != nullptr, "deep_clone_stmt returns a node");
    if (deep != nullptr) expect_stmt_round_trip(*deep, "deep_clone_stmt");

    const scpp::Stmt copy_constructed{original};
    expect_stmt_round_trip(copy_constructed, "Stmt copy constructor");

    scpp::Stmt copy_assigned{};
    copy_assigned = original;
    expect_stmt_round_trip(copy_assigned, "Stmt copy assignment");

    if (deep != nullptr && deep->then_branch != nullptr) deep->then_branch->var_name = "mutated";
    expect(original.then_branch != nullptr && original.then_branch->var_name == "then_child",
           "deep_clone_stmt does not share children with the original");
}

// A `Stmt` reached only through an enclosing node's *copy constructor* used to
// go through a different, incomplete cloner: `clone_initializer_stmt` had no
// `switch_cases` loop at all, so a global variable or default-argument lambda
// body containing a `switch` lost every case. Both paths now share one
// enumeration, so exercise the indirect one explicitly.
void test_global_var_copy_preserves_switch_cases() {
    scpp::GlobalVar original{};
    original.decl = std::make_unique<scpp::Stmt>();
    original.decl->kind = scpp::StmtKind::VarDecl;
    original.decl->var_name = "g";
    original.decl->init = std::make_unique<scpp::Expr>();
    original.decl->init->kind = scpp::ExprKind::Lambda;
    original.decl->init->lambda_body = std::make_unique<scpp::Stmt>();
    original.decl->init->lambda_body->kind = scpp::StmtKind::Switch;
    original.decl->init->lambda_body->is_const = true;
    original.decl->init->lambda_body->is_static_local = true;
    original.decl->init->lambda_body->resolved_alignment = 16;
    scpp::SwitchCase only_case{};
    only_case.value = std::make_unique<scpp::Expr>();
    only_case.value->kind = scpp::ExprKind::IntegerLiteral;
    only_case.value->int_value = 5;
    only_case.statements.push_back(std::make_unique<scpp::Stmt>());
    only_case.statements[0]->kind = scpp::StmtKind::Break;
    original.decl->init->lambda_body->switch_cases.push_back(std::move(only_case));

    const scpp::GlobalVar copy{original};
    expect(copy.decl != nullptr && copy.decl->init != nullptr && copy.decl->init->lambda_body != nullptr,
           "GlobalVar's copy constructor clones the declaration");
    if (copy.decl == nullptr || copy.decl->init == nullptr || copy.decl->init->lambda_body == nullptr) return;
    const scpp::Stmt& body = *copy.decl->init->lambda_body;
    expect(body.switch_cases.size() == 1, "an indirectly cloned Stmt keeps its switch cases");
    expect(body.switch_cases.size() == 1 && body.switch_cases[0].value != nullptr &&
               body.switch_cases[0].value->int_value == 5 && body.switch_cases[0].statements.size() == 1,
           "an indirectly cloned switch case keeps its value and body");
    expect(body.is_const, "an indirectly cloned Stmt keeps is_const");
    expect(body.is_static_local, "an indirectly cloned Stmt keeps is_static_local");
    expect(body.resolved_alignment == 16, "an indirectly cloned Stmt keeps resolved_alignment");
}

// Codegen materializes each lambda capture by evaluating an `Identifier` that
// names the captured entity in the enclosing scope. That node is synthesized,
// not cloned, so it is a second way for a resolution field to go missing --
// there is now one factory for it.
void test_capture_identifier_carries_resolution() {
    scpp::LambdaCapture capture{};
    capture.name = "captured";
    capture.by_reference = true;
    capture.resolved_local = 88;
    const scpp::Expr ident = scpp::make_capture_identifier(capture, scpp::SourceLocation{4, 9});
    expect(ident.kind == scpp::ExprKind::Identifier, "a capture identifier is an Identifier");
    expect(ident.name == "captured", "a capture identifier carries the captured name");
    expect(ident.resolved_local == 88, "a capture identifier carries the enclosing local's id");
    expect(ident.loc.line == 4 && ident.loc.column == 9, "a capture identifier carries the requested location");
}

} // namespace

int main() {
    test_int_main_return();
    test_function_with_params();
    test_var_decl_and_if_else();
    test_class_var_decl_with_brace_init_parses_ctor_args();
    test_class_var_decl_with_paren_init_is_rejected();
    test_bare_local_var_decl_is_rejected();
    test_static_local_var_decl_parses_and_allows_no_initializer();
    test_fixed_width_integer_keywords_and_std_qualification_parse();
    test_size_t_and_ptrdiff_t_keywords_and_std_qualification_parse();
    test_valid_local_initializer_forms_parse();
    test_class_default_member_initializers_parse();
    test_constructor_member_initializer_list_parses();
    test_constructor_base_initializer_list_parses();
    test_while_loop();
    test_classic_for_loop_desugars_with_scoped_init();
    test_classic_for_loop_with_expression_init_desugars();
    test_range_for_loop_desugars_over_array();
    test_range_for_loop_desugars_over_span();
    test_range_for_loop_named_element_forms_desugar();
    test_range_for_body_parse_errors_are_not_misattributed_to_loop_var();
    test_break_and_continue_parse_inside_loop();
    test_break_outside_loop_is_rejected();
    test_continue_outside_loop_is_rejected();
    test_switch_statement_parses_with_cases_default_and_fallthrough();
    test_braced_switch_case_body_ending_in_terminator_is_accepted();
    test_nested_braced_switch_case_bodies_compose();
    test_empty_braced_switch_case_body_is_rejected();
    test_braced_switch_case_body_without_terminator_is_rejected();
    test_braced_switch_case_body_ending_in_if_else_is_rejected();
    test_fallthrough_nested_in_braced_switch_case_body_is_rejected();
    test_top_level_fallthrough_after_braced_body_is_still_accepted();
    test_bare_switch_case_without_terminator_is_still_rejected();
    test_namespaced_enum_case_label_resolves_to_qualified_enumerator();
    test_break_parses_inside_switch();
    test_fallthrough_outside_switch_is_rejected();
    test_fallthrough_must_be_last_in_case();
    test_switch_case_requires_explicit_terminator();
    test_grouped_switch_case_labels_parse_as_empty_cases();
    test_unsafe_block_sets_is_unsafe_flag();
    test_ordinary_block_is_not_unsafe();
    test_nested_unsafe_blocks_parse();
    test_enum_class_declaration_parses();
    test_enum_class_underlying_type_parses();
    test_old_style_enum_is_rejected();
    test_parse_returns_engaged_expected_on_success();
    test_parse_returns_disengaged_expected_on_failure_without_throwing();
    test_bare_unsafe_identifier_followed_by_return_is_parse_error();
    test_unsafe_attribute_on_non_block_statement_has_no_effect();
    test_function_level_unsafe_marker_parses();
    test_nodiscard_function_and_method_attributes_parse();
    test_inline_function_modifier_parses_with_existing_modifiers();
    test_default_parameter_expression_parses();
    test_default_parameter_trailing_rule_is_enforced();
    test_static_member_function_parses_without_this();
    test_template_specialization_static_member_call_parses();
    test_full_class_template_specialization_parses_as_concrete_specialization();
    test_struct_forward_declaration_parses_and_reconciles();
    test_class_forward_declaration_parses_and_reconciles();
    test_record_forward_declaration_tag_mismatch_is_rejected();
    test_unsafe_attribute_on_struct_is_rejected();
    test_thread_safety_attribute_on_struct_parses();
    test_thread_safety_attributes_on_class_parse();
    test_thread_safety_attribute_on_parameter_parses();
    test_user_declared_move_constructor_is_rejected();
    test_constructor_taking_other_type_rvalue_reference_parses();
    test_operator_assign_parses();
    test_out_of_line_constructor_definition_parses_and_merges();
    test_out_of_line_destructor_definition_parses_and_merges();
    test_out_of_line_operator_assign_definition_parses_and_merges();
    test_out_of_line_member_definition_signature_mismatch_is_rejected();
    test_user_declared_move_assignment_operator_is_rejected();
    test_defaulted_move_special_members_parse_without_parameter_names();
    test_defaulted_non_special_member_is_rejected();
    test_equality_operator_methods_parse();
    test_static_cast_parses();
    test_c_style_cast_parses();
    test_parenthesized_expression_is_not_misdetected_as_cast();
    test_sizeof_type_expression_parses();
    test_sizeof_value_expression_parses();
    test_conditional_expression_parses();
    test_operator_precedence();
    test_unary_and_call();
    test_dereference_expression();
    test_address_of_plain_variable();
    test_address_of_field_and_subscript();
    test_address_of_dereference_chain();
    test_increment_and_decrement_operators_parse();
    test_compound_assignment_operators_parse();
    test_local_type_definitions_parse();
    test_arrow_parses_as_deferred_operator_arrow_access();
    test_chained_arrow_and_dot();
    test_operator_arrow_member_decl_and_explicit_call_parse();
    test_member_function_lifetime_this_parse();
    test_member_decl_return_lifetime_this_parse();
    test_return_brace_constructed_optional_reference_wrapper_parses_as_call();
    test_multiplication_is_not_confused_with_dereference();
    test_parenthesized_expression();
    test_parse_error_on_missing_semicolon();
    test_struct_declaration();
    test_struct_access_specifier_sections_parse();
    test_struct_constructors_and_methods_parse();
    test_struct_interface_attribute_is_rejected();
    test_struct_base_clause_is_rejected();
    test_struct_virtual_member_is_rejected();
    test_union_declaration();
    test_packed_struct_and_union_attributes_parse();
    test_packed_attribute_on_function_is_rejected();
    test_struct_variable_and_member_access();
    test_nested_member_access();
    test_pointer_field_type();
    test_array_field_and_subscript();
    test_array_parameter_decays_to_pointer();
    test_local_array_declaration();
    test_struct_before_use_is_required();
    test_unique_ptr_type_declaration();
    test_unique_ptr_of_struct_type();
    test_span_type_declaration();
    test_std_string_view_type_and_calls_parse();
    test_span_of_const_element_type();
    test_move_expression();
    test_move_as_function_argument();
    test_brace_init_return_expression();
    test_make_unique_zero_args();
    test_make_unique_with_arg();
    test_make_unique_of_struct_type();
    test_new_and_delete_parse();
    test_placement_new_parse();
    test_explicit_destructor_parse();
    test_extern_c_single_declaration();
    test_extern_c_block_form();
    test_extern_c_definition_is_checked_like_any_function();
    test_extern_cpp_linkage_is_rejected();
    test_extern_c_varargs_declaration();
    test_extern_c_function_pointer_parameter_declaration();
    test_varargs_on_definition_is_rejected();
    test_varargs_on_non_extern_function_is_rejected();
    test_void_return_and_void_pointer_types();
    test_char_type_declaration();
    test_char_literal_expression();
    test_char_literal_escape_sequences_decode_correctly();
    test_empty_char_literal_is_rejected();
    test_multi_character_char_literal_is_rejected();
    test_unsupported_char_escape_is_rejected();
    test_string_literal_expression();
    test_string_literal_escape_sequences_decode_correctly();
    test_empty_string_literal_is_allowed();
    test_unsupported_string_escape_is_rejected();
    test_const_char_pointer_type();
    test_plain_pointer_defaults_to_mutable_pointee();
    test_const_local_variable_parses();
    test_ordinary_local_variable_is_not_const();
    test_const_local_variable_without_initializer_is_rejected();
    test_constexpr_function_parses();
    test_consteval_function_parses();
    test_constexpr_constructor_parses();
    test_constexpr_local_variable_parses();
    test_constexpr_local_variable_without_initializer_is_rejected();
    test_if_consteval_parses();
    test_if_not_consteval_parses();
    test_export_module_declaration();
    test_dotted_module_name_declaration();
    test_plain_module_declaration_is_implementation_unit();
    test_global_module_fragment_before_interface_module_declaration();
    test_global_module_fragment_before_partition_declaration();
    test_global_module_fragment_without_following_module_declaration_is_rejected();
    test_no_module_declaration_leaves_module_name_empty();
    test_namespace_qualifies_struct_name();
    test_nested_namespace_one_liner_qualifies_function_name();
    test_qualified_type_reference_parses();
    test_export_prefix_marks_function_exported();
    test_no_export_prefix_leaves_function_not_exported();
    test_export_group_marks_multiple_declarations_exported();
    test_export_namespace_block_marks_direct_members_exported();
    test_export_namespace_block_matches_per_declaration_exports();
    test_export_class_propagates_to_methods();
    test_export_in_non_matching_namespace_is_allowed();
    test_export_with_no_namespace_is_allowed();
    test_export_in_deeper_nested_namespace_is_allowed();
    test_export_without_any_module_declaration_is_rejected();
    test_bare_extern_declaration_is_module_extern();
    test_bare_extern_declaration_is_namespace_qualified();
    test_module_forward_declarations_reconcile_to_definitions();
    test_module_forward_declaration_mismatched_definition_is_rejected();
    test_partition_declaration_sets_partition_name();
    test_implementation_partition_declaration();
    test_partition_import_outside_module_is_rejected();
    test_partition_import_without_resolver_is_rejected();
    test_partition_import_merges_with_body();
    test_plain_partition_import_does_not_reexport();
    test_export_import_on_implementation_partition_is_rejected();
    test_rvalue_reference_parameter_parses();
    test_ordinary_reference_parameter_is_not_rvalue_ref();
    test_const_rvalue_reference_is_rejected();
    test_rvalue_reference_rejected_outside_parameter_position();
    test_concept_compound_requirement_synthesizes_witness_class();
    test_concept_simple_direct_invocation_requirement_synthesizes_call_method();
    test_concept_requirement_on_wrong_receiver_is_rejected();
    test_concept_convertible_to_constraint_is_rejected();
    test_concept_requirement_unknown_argument_is_rejected();
    test_export_concept_outside_module_is_rejected();
    test_concept_inside_namespace_is_qualified();
    test_return_brace_constructed_optional_reference_wrapper_parses_as_call();
    test_nested_reference_wrapper_lifetime_parameter_parse();
    test_generic_type_brace_init_return_expression_parses();
    test_generic_type_declaration_brace_init_still_parses_as_ctor_args();
    test_generic_type_ctad_brace_init_return_expression_parses();
    test_generic_parameter_const_auto_ref_parses();
    test_generic_parameter_auto_rvalue_ref_parses();
    test_generic_parameter_mutable_auto_ref_parses();
    test_bare_auto_parameter_parses();
    test_bare_auto_lambda_parameter_parses();
    test_generic_parameter_unknown_concept_is_rejected();
    test_generic_parameter_on_method_is_rejected();
    test_lambda_with_explicit_captures_parses();
    test_lambda_blanket_capture_modes_parse();
    test_lambda_init_capture_parses();
    test_lambda_this_and_star_this_captures_parse();
    test_function_pointer_declarators_parse();
    test_lambda_generic_parameter_is_rejected();
    test_lambda_mutable_keyword_parses();
    test_generic_class_bare_type_param_parses();
    test_generic_class_multiple_type_params_parse();
    test_generic_class_named_pack_method_params_parse();
    test_generic_class_named_pack_function_pointer_params_parse();
    test_class_member_templates_parse();
    test_function_type_template_argument_parses();
    test_qualified_function_type_template_argument_parses();
    test_ref_qualified_methods_parse();
    test_explicit_template_function_designator_parses();
    test_global_qualified_call_parses();
    test_class_partial_specialization_on_function_type_parses();
    test_generic_class_method_requires_clause_parses();
    test_generic_struct_concept_constrained_type_param_parses();
    test_generic_struct_bare_type_param_is_rejected();
    test_generic_type_instantiation_parses_with_template_args();
    test_class_public_inheritance_parses();
    test_class_inheritance_defaults_to_private();
    test_class_inheritance_from_undeclared_class_is_rejected();
    test_interface_attribute_sets_class_flag();
    test_multiple_base_specifiers_parse_with_access_and_virtual_flags();
    test_class_scope_using_declaration_parses();
    test_virtual_override_pure_and_defaulted_member_flags_parse();
    test_override_without_virtual_member_flags_parse();
    test_variadic_primary_template_decl_parses();
    test_variadic_empty_pack_specialization_parses();
    test_variadic_recursive_specialization_parses();
    test_variadic_specialization_member_names_include_owner_id();
    test_variadic_instantiation_with_multiple_args_parses();
    test_variadic_struct_is_rejected();
    test_variadic_specialization_without_primary_is_rejected();
    test_generic_function_full_header_form_parses();
    test_generic_function_multiple_type_params_parses();
    test_full_header_parameter_pack_and_new_pack_expansion_parse();
    test_full_header_transformed_pointer_parameter_pack_parses();
    test_full_header_wrapped_template_parameter_pack_parses();
    test_abbreviated_generic_parameter_pack_and_fold_parse();
    test_explicit_type_template_argument_call_parses();
    test_const_qualified_template_type_argument_parses();
    test_const_qualified_explicit_template_argument_parses();
    test_explicit_template_argument_call_with_multiple_value_args_parses();
    test_explicit_non_type_template_argument_call_parses();
    test_variadic_specialization_with_leading_non_type_param_parses();
    test_phase1_ast_metadata_fields_are_storable();
    test_namespace_relative_qualified_generic_type_declaration_parses();
    test_type_alias_declaration_parses_and_resolves();
    test_exported_type_alias_inside_namespace_parses();

    test_expr_field_count_is_guarded();
    test_stmt_field_count_is_guarded();
    test_lambda_capture_field_count_is_guarded();
    test_switch_case_field_count_is_guarded();
    test_explicit_template_arg_field_count_is_guarded();
    test_param_field_count_is_guarded();
    test_expr_clone_preserves_every_field();
    test_stmt_clone_preserves_every_field();
    test_global_var_copy_preserves_switch_cases();
    test_capture_identifier_carries_resolution();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All parser tests passed.\n";
    return 0;
}
