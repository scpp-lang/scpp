import scpp.parser;
import scpp.ast;

#include <fstream>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

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

void test_int_main_return() {
    scpp::Program program = scpp::parse("int main() { return 42; }");
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
    scpp::Program program = scpp::parse("int add(int a, int b) { return a + b; }");
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
    scpp::Program program = scpp::parse(
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

void test_while_loop() {
    scpp::Program program = scpp::parse("int f() { while (true) { x = x - 1; } }");
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

void test_unsafe_block_sets_is_unsafe_flag() {
    // `[[scpp::unsafe]] { }` (ch01 §1.3) is an ordinary Block statement
    // with is_unsafe set -- see parse_statement's attribute handling.
    scpp::Program program = scpp::parse("int f() { [[scpp::unsafe]] { int x = 1; } return 0; }");
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
    scpp::Program program = scpp::parse("int f() { { int x = 1; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& plain_block = *fn.body->statements[0];
    expect(plain_block.kind == scpp::StmtKind::Block, "ordinary_block_is_not_unsafe: should be a Block");
    expect(!plain_block.is_unsafe, "ordinary_block_is_not_unsafe: is_unsafe should be false");
}

void test_nested_unsafe_blocks_parse() {
    // `[[scpp::unsafe]] { [[scpp::unsafe]] { ... } }` (ch01 §1.3's
    // nesting rule) -- both levels independently set is_unsafe.
    scpp::Program program =
        scpp::parse("int f() { [[scpp::unsafe]] { [[scpp::unsafe]] { int x = 1; } } return 0; }");
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
    try {
        scpp::parse("int f() { unsafe return 1; }");
    } catch (const scpp::ParseError&) {
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
    scpp::Program program = scpp::parse("int f() { [[scpp::unsafe]] return 1; }");
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
    scpp::Program program = scpp::parse("[[scpp::unsafe]] int f(int x) { return x; }\n"
                                         "int main() { return 0; }\n");
    const scpp::Function* f_fn = nullptr;
    for (const scpp::Function& fn : program.functions) {
        if (fn.name == "f") f_fn = &fn;
    }
    expect(f_fn != nullptr, "function_level_unsafe_marker_parses: expected a Function named 'f'");
    expect(f_fn->is_unsafe, "function_level_unsafe_marker_parses: is_unsafe should be true");
}

// ch01 §1.3 (1): `[[scpp::unsafe]]` may only appertain to a compound-
// statement or a function's own declaration -- appertaining to a
// struct/class declaration is ill-formed.
void test_unsafe_attribute_on_struct_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("[[scpp::unsafe]] struct Foo { int x; };\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "unsafe_attribute_on_struct_is_rejected: expected a ParseError");
}

// ch05 §5.15: `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]` on a
// struct's own declaration set the manual-override flags.
void test_thread_safety_attribute_on_struct_parses() {
    scpp::Program program = scpp::parse(
        "struct [[scpp::thread_movable]] RawBufferHandle { int* data; int len; };\n"
        "int main() { return 0; }\n");
    const scpp::StructDef* s = nullptr;
    for (const scpp::StructDef& def : program.structs) {
        if (def.name == "RawBufferHandle") s = &def;
    }
    expect(s != nullptr, "thread_safety_attribute_on_struct_parses: expected a StructDef named 'RawBufferHandle'");
    expect(s->thread_movable_override, "thread_safety_attribute_on_struct_parses: thread_movable_override should be true");
    expect(!s->thread_shareable_override, "thread_safety_attribute_on_struct_parses: thread_shareable_override should be false");
}

// Same attribute grammar slot on a class's own declaration; both
// attributes may be given together, comma-separated inside one `[[...]]`.
void test_thread_safety_attributes_on_class_parse() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("class Foo {\n"
                    "public:\n"
                    "    Foo() { return; }\n"
                    "    Foo(Foo&& other) { return; }\n"
                    "};\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "user_declared_move_constructor_is_rejected: expected a ParseError");
}

// An ordinary constructor taking a *different* type's rvalue reference
// (not the enclosing class's own) is not a move constructor at all, and
// must continue to parse normally.
void test_constructor_taking_other_type_rvalue_reference_parses() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("class Foo {\n"
                    "public:\n"
                    "    Foo() { return; }\n"
                    "    Foo& operator=(Foo&& other) { return this; }\n"
                    "};\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "user_declared_move_assignment_operator_is_rejected: expected a ParseError");
}

// ch06 §6: `static_cast<T>(expr)` parses to a Cast expression with `type`
// set to the target type and the operand in `lhs`.
void test_static_cast_parses() {
    scpp::Program program = scpp::parse("int f(bool b) { return static_cast<int>(b); }");
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
    scpp::Program program = scpp::parse("int f(bool b) { return (int)b; }");
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
    scpp::Program program = scpp::parse("int f(int x, int y) { return (x) + (x + y) * 2; }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    expect(ret.expr->kind == scpp::ExprKind::Binary,
           "parenthesized_expression_is_not_misdetected_as_cast: expected Binary");
}

void test_sizeof_type_expression_parses() {
    scpp::Program program = scpp::parse("int f() { return (int)sizeof(int); }");
    const scpp::Stmt& ret = *program.functions[0].body->statements[0];
    const scpp::Expr& cast = *ret.expr;
    expect(cast.kind == scpp::ExprKind::Cast, "sizeof_type_expression_parses: expected outer Cast");
    expect(cast.lhs != nullptr && cast.lhs->kind == scpp::ExprKind::Sizeof,
           "sizeof_type_expression_parses: cast operand should be Sizeof");
    expect(cast.lhs->sizeof_operand_is_type, "sizeof_type_expression_parses: expected sizeof(type) form");
    expect(is_named_type(cast.lhs->type, "int"), "sizeof_type_expression_parses: queried type should be 'int'");
}

void test_sizeof_value_expression_parses() {
    scpp::Program program = scpp::parse("int f() { return (int)sizeof(x + 1); }");
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
    scpp::Program program = scpp::parse("int f() { return 1 + 2 * 3; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0].get()->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Add,
           "operator_precedence: top-level op should be Add");
    expect(expr.lhs->kind == scpp::ExprKind::IntegerLiteral && expr.lhs->int_value == 1,
           "operator_precedence: lhs should be IntegerLiteral 1");
    expect(expr.rhs->kind == scpp::ExprKind::Binary && expr.rhs->binary_op == scpp::BinaryOp::Mul,
           "operator_precedence: rhs should be Mul");
}

void test_unary_and_call() {
    scpp::Program program = scpp::parse("int f() { return -foo(1, 2); }");
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
    scpp::Program program = scpp::parse("int f() { return *p; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::Deref,
           "dereference_expression: top-level should be unary Deref");
    expect(expr.lhs->kind == scpp::ExprKind::Identifier && expr.lhs->name == "p",
           "dereference_expression: operand should be identifier 'p'");
}

void test_address_of_plain_variable() {
    // `&x` (ch05 §5.7) -- a new prefix unary operator, sibling to Deref.
    scpp::Program program = scpp::parse("int f() { return &x; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_plain_variable: top-level should be unary AddressOf");
    expect(expr.lhs->kind == scpp::ExprKind::Identifier && expr.lhs->name == "x",
           "address_of_plain_variable: operand should be identifier 'x'");
}

void test_address_of_field_and_subscript() {
    // `&p.x` and `&arr[i]` -- same operand shapes already accepted as a
    // borrow source for `T&`/`const T&` (ch05.2), reused here.
    scpp::Program field_program = scpp::parse("int f() { return &p.x; }");
    const scpp::Expr& field_expr = *field_program.functions[0].body->statements[0]->expr;
    expect(field_expr.kind == scpp::ExprKind::Unary && field_expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_field_and_subscript: &p.x top-level should be unary AddressOf");
    expect(field_expr.lhs->kind == scpp::ExprKind::Member && field_expr.lhs->name == "x",
           "address_of_field_and_subscript: &p.x operand should be Member 'x'");

    scpp::Program subscript_program = scpp::parse("int f() { return &arr[i]; }");
    const scpp::Expr& subscript_expr = *subscript_program.functions[0].body->statements[0]->expr;
    expect(subscript_expr.kind == scpp::ExprKind::Unary && subscript_expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_field_and_subscript: &arr[i] top-level should be unary AddressOf");
    expect(subscript_expr.lhs->kind == scpp::ExprKind::Subscript,
           "address_of_field_and_subscript: &arr[i] operand should be Subscript");
}

void test_address_of_dereference_chain() {
    // `&*p` -- address-of applied to a dereference, recursing off Deref
    // just like Neg/Not/Deref's own operands already do (parse_unary).
    scpp::Program program = scpp::parse("int f() { return &*p; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Unary && expr.unary_op == scpp::UnaryOp::AddressOf,
           "address_of_dereference_chain: top-level should be unary AddressOf");
    expect(expr.lhs->kind == scpp::ExprKind::Unary && expr.lhs->unary_op == scpp::UnaryOp::Deref,
           "address_of_dereference_chain: operand should be unary Deref");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Identifier && expr.lhs->lhs->name == "p",
           "address_of_dereference_chain: innermost operand should be identifier 'p'");
}

void test_arrow_desugars_to_member_of_deref() {
    // `p->x` is sugar for `(*p).x`, same as real C++ -- see parse_postfix.
    scpp::Program program = scpp::parse("int f() { return p->x; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Member && expr.name == "x",
           "arrow_desugars_to_member_of_deref: top-level should be Member 'x'");
    expect(expr.lhs->kind == scpp::ExprKind::Unary && expr.lhs->unary_op == scpp::UnaryOp::Deref,
           "arrow_desugars_to_member_of_deref: base should be unary Deref");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Identifier && expr.lhs->lhs->name == "p",
           "arrow_desugars_to_member_of_deref: deref operand should be identifier 'p'");
}

void test_chained_arrow_and_dot() {
    // `p->x.y` chains a `->` (deref+member) followed by a plain `.`.
    scpp::Program program = scpp::parse("int f() { return p->x.y; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Member && expr.name == "y",
           "chained_arrow_and_dot: outer should be Member 'y'");
    expect(expr.lhs->kind == scpp::ExprKind::Member && expr.lhs->name == "x",
           "chained_arrow_and_dot: middle should be Member 'x'");
    expect(expr.lhs->lhs->kind == scpp::ExprKind::Unary && expr.lhs->lhs->unary_op == scpp::UnaryOp::Deref,
           "chained_arrow_and_dot: inner should be unary Deref");
}

void test_multiplication_is_not_confused_with_dereference() {
    // `a * b` (binary multiply) must stay distinct from a leading `*b`
    // (unary deref) -- see parse_unary's comment.
    scpp::Program program = scpp::parse("int f() { return a * b; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Mul,
           "multiplication_is_not_confused_with_dereference: should be Binary Mul, not Unary Deref");
}

void test_parenthesized_expression() {
    // (1 + 2) * 3 should parse with the addition grouped first.
    scpp::Program program = scpp::parse("int f() { return (1 + 2) * 3; }");
    const scpp::Expr& expr = *program.functions[0].body->statements[0]->expr;
    expect(expr.kind == scpp::ExprKind::Binary && expr.binary_op == scpp::BinaryOp::Mul,
           "parenthesized_expression: top-level op should be Mul");
    expect(expr.lhs->kind == scpp::ExprKind::Binary && expr.lhs->binary_op == scpp::BinaryOp::Add,
           "parenthesized_expression: lhs should be Add");
}

void test_parse_error_on_missing_semicolon() {
    bool threw = false;
    try {
        scpp::parse("int f() { return 1 }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "parse_error_on_missing_semicolon: expected a ParseError to be thrown");
}

void test_struct_declaration() {
    scpp::Program program = scpp::parse("struct Point { int x; int y; }; int f() { return 0; }");
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

void test_union_declaration() {
    scpp::Program program = scpp::parse("union Payload { int i; char c; }; int f() { return 0; }");
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
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("[[scpp::packed]]\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError& e) {
        threw = true;
        expect(std::string(e.what()).find("only to a struct or union declaration") != std::string::npos,
               "packed_attribute_on_function_is_rejected: diagnostic should mention struct/union-only support");
    }
    expect(threw, "packed_attribute_on_function_is_rejected: expected a ParseError");
}

void test_struct_variable_and_member_access() {
    scpp::Program program = scpp::parse(
        "struct Point { int x; int y; };"
        "int f() {"
        "    Point p;"
        "    p.x = 1;"
        "    return p.x + p.y;"
        "}");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "struct_variable_and_member_access: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "Point" && decl.var_name == "p",
           "struct_variable_and_member_access: decl should be 'Point p'");
    expect(decl.init == nullptr, "struct_variable_and_member_access: no initializer given");

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
    scpp::Program program = scpp::parse(
        "struct Inner { int v; };"
        "struct Outer { Inner inner; };"
        "int f() {"
        "    Outer o;"
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
    scpp::Program program = scpp::parse("struct Node { int value; Node* next; };");
    const scpp::StructDef& def = program.structs[0];
    expect(def.fields[1].name == "next", "pointer_field_type: field 1 should be named 'next'");
    const scpp::Type& next_type = def.fields[1].type;
    expect(next_type.kind == scpp::TypeKind::Pointer, "pointer_field_type: field 1 should be a Pointer type");
    expect(next_type.pointee != nullptr && is_named_type(*next_type.pointee, "Node"),
           "pointer_field_type: pointee should be named 'Node'");
}

void test_array_field_and_subscript() {
    scpp::Program program = scpp::parse(
        "struct Buffer { int values[4]; };"
        "int f() {"
        "    Buffer b;"
        "    b.values[0] = 1;"
        "    return b.values[0];"
        "}");
    const scpp::StructDef& def = program.structs[0];
    const scpp::Type& values_type = def.fields[0].type;
    expect(values_type.kind == scpp::TypeKind::Array, "array_field_and_subscript: field should be an Array type");
    expect(values_type.array_size == 4, "array_field_and_subscript: array size should be 4");
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
    scpp::Program program = scpp::parse("int f(int arr[4]) { return arr[0]; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 1, "array_parameter_decays_to_pointer: expected 1 parameter");
    const scpp::Type& param_type = fn.params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer,
           "array_parameter_decays_to_pointer: parameter type should be Pointer, not Array");
    expect(param_type.pointee != nullptr && is_named_type(*param_type.pointee, "int"),
           "array_parameter_decays_to_pointer: pointee should be 'int'");
}

void test_local_array_declaration() {
    scpp::Program program = scpp::parse("int f() { int values[8]; return values[0]; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "local_array_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Array && decl.type.array_size == 8,
           "local_array_declaration: type should be an Array of size 8");
    expect(decl.type.element != nullptr && is_named_type(*decl.type.element, "int"),
           "local_array_declaration: element type should be 'int'");
}

void test_struct_before_use_is_required() {
    bool threw = false;
    try {
        scpp::parse("int f() { Point p; return 0; } struct Point { int x; };");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "struct_before_use_is_required: expected a ParseError when Point is used before declaration");
}

void test_unique_ptr_type_declaration() {
    scpp::Program program = parse_with_std_imports("import std;\nint f() { std::unique_ptr<int> a; return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "unique_ptr_type_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "std::unique_ptr",
           "unique_ptr_type_declaration: type should be std::unique_ptr");
    expect(decl.type.template_args.size() == 1 && is_named_type(decl.type.template_args[0], "int"),
           "unique_ptr_type_declaration: pointee should be 'int'");
    expect(decl.var_name == "a", "unique_ptr_type_declaration: variable name should be 'a'");
    expect(decl.init == nullptr, "unique_ptr_type_declaration: no initializer given");
}

void test_unique_ptr_of_struct_type() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "struct Point { int x; int y; };"
        "int f() { std::unique_ptr<Point> a; return 0; }");
    const scpp::Function& fn = *find_function_named(program, "f");
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "std::unique_ptr",
           "unique_ptr_of_struct_type: type should be std::unique_ptr");
    expect(decl.type.template_args.size() == 1 && is_named_type(decl.type.template_args[0], "Point"),
           "unique_ptr_of_struct_type: pointee should be 'Point'");
}

void test_span_type_declaration() {
    scpp::Program program = scpp::parse("int f() { int arr[3]; std::span<int> s = arr; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "span_type_declaration: statement 1 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Span, "span_type_declaration: type should be Span");
    expect(decl.type.pointee != nullptr && is_named_type(*decl.type.pointee, "int"),
           "span_type_declaration: element type should be 'int'");
    expect(decl.type.is_mutable_ref, "span_type_declaration: std::span<int> should be mutable (is_mutable_ref)");
    expect(decl.var_name == "s", "span_type_declaration: variable name should be 's'");
}

void test_span_of_const_element_type() {
    scpp::Program program = scpp::parse("int f() { int arr[3]; std::span<const int> s = arr; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.type.kind == scpp::TypeKind::Span, "span_of_const_element_type: type should be Span");
    expect(!decl.type.is_mutable_ref,
           "span_of_const_element_type: std::span<const int> should be read-only (!is_mutable_ref)");
}

void test_storage_for_type_declaration() {
    scpp::Program program =
        scpp::parse("int f() { std::storage_for<int, long> slot; return (int)sizeof(slot); }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "storage_for_type_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::Named && decl.type.name == "std::storage_for",
           "storage_for_type_declaration: type should be std::storage_for");
    expect(decl.type.template_args.size() == 2, "storage_for_type_declaration: expected 2 type arguments");
    expect(is_named_type(decl.type.template_args[0], "int"), "storage_for_type_declaration: arg 0 should be int");
    expect(is_named_type(decl.type.template_args[1], "long"), "storage_for_type_declaration: arg 1 should be long");
}

void test_move_expression() {
    scpp::Program program = parse_with_std_imports(
        "import std;\n"
        "int f() {"
        "    std::unique_ptr<int> a;"
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
        "    std::unique_ptr<int> a;"
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
        "int f() { [[scpp::unsafe]] { std::storage_for<int> slot; int* p = new ((int*)&slot) int(7); } return 0; }");
    const scpp::Stmt& unsafe_block = *program.functions[0].body->statements[0];
    const scpp::Stmt& decl = *unsafe_block.statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "placement_new_parse: statement 1 should be VarDecl");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::New,
           "placement_new_parse: initializer should be a New expression");
    expect(decl.init->lhs != nullptr && decl.init->lhs->kind == scpp::ExprKind::Cast,
           "placement_new_parse: placement operand should parse as a Cast");
    expect(decl.init->has_paren_init, "placement_new_parse: placement new should preserve ctor paren-init");
}

void test_full_header_parameter_pack_and_new_pack_expansion_parse() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse("extern \"C\" int c_abs(int n); int main() { return 0; }");
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse("extern \"C\" int add(int a, int b) { return a + b; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_extern_c, "extern_c_definition_is_checked_like_any_function: is_extern_c should be true");
    expect(fn.body != nullptr, "extern_c_definition_is_checked_like_any_function: body should be present");
}

void test_extern_cpp_linkage_is_rejected() {
    // v0.1 only accepts the literal "C" linkage string.
    bool threw = false;
    try {
        scpp::parse("extern \"C++\" int foo(int x);");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "extern_cpp_linkage_is_rejected: expected a ParseError");
}

void test_extern_c_varargs_declaration() {
    // ch02 §2.1: `...` is parsed and stored as has_varargs on a bodyless
    // extern "C" declaration.
    scpp::Program program = scpp::parse("extern \"C\" int my_printf(int fmt, ...);");
    const scpp::Function& fn = program.functions[0];
    expect(fn.has_varargs, "extern_c_varargs_declaration: has_varargs should be true");
    expect(fn.params.size() == 1, "extern_c_varargs_declaration: expected exactly 1 named parameter");
}

void test_extern_c_function_pointer_parameter_declaration() {
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("extern \"C\" int f(int a, ...) { return a; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "varargs_on_definition_is_rejected: expected a ParseError");
}

void test_varargs_on_non_extern_function_is_rejected() {
    // `...` is only meaningful for extern "C" declarations (ch02 §2.1).
    bool threw = false;
    try {
        scpp::parse("int f(int a, ...);");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "varargs_on_non_extern_function_is_rejected: expected a ParseError");
}

void test_void_return_and_void_pointer_types() {
    // ch02 §2.1's `void` prerequisite: valid as a return type and as a
    // pointer's pointee.
    scpp::Program program = scpp::parse("extern \"C\" void free(void* p);");
    const scpp::Function& fn = program.functions[0];
    expect(is_named_type(fn.return_type, "void"), "void_return_and_void_pointer_types: return type should be 'void'");
    expect(fn.params.size() == 1 && fn.params[0].type.kind == scpp::TypeKind::Pointer,
           "void_return_and_void_pointer_types: parameter should be a pointer");
    expect(is_named_type(*fn.params[0].type.pointee, "void"),
           "void_return_and_void_pointer_types: parameter's pointee should be 'void'");
}

void test_char_type_declaration() {
    scpp::Program program = scpp::parse("int f() { char c; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "char_type_declaration: statement should be VarDecl");
    expect(is_named_type(decl.type, "char"), "char_type_declaration: type should be 'char'");
}

void test_char_literal_expression() {
    scpp::Program program = scpp::parse("int f() { char c = 'a'; return 0; }");
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
        scpp::Program program = scpp::parse(c.source);
        const scpp::Stmt& decl = *program.functions[0].body->statements[0];
        expect(decl.init->int_value == c.expected,
               "char_literal_escape_sequences_decode_correctly: mismatch for " + std::string(c.source));
    }
}

void test_empty_char_literal_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f() { char c = ''; return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "empty_char_literal_is_rejected: expected a ParseError");
}

void test_multi_character_char_literal_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f() { char c = 'ab'; return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "multi_character_char_literal_is_rejected: expected a ParseError");
}

void test_unsupported_char_escape_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f() { char c = '\\z'; return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "unsupported_char_escape_is_rejected: expected a ParseError");
}

void test_string_literal_expression() {
    scpp::Program program = scpp::parse("int f(char* p) { p = \"hello\"; return 0; }");
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
        scpp::Program program = scpp::parse(c.source);
        const scpp::Expr& assign = *program.functions[0].body->statements[0]->expr;
        expect(assign.rhs->name == c.expected,
               "string_literal_escape_sequences_decode_correctly: mismatch for " + std::string(c.source));
    }
}

void test_empty_string_literal_is_allowed() {
    // Unlike an empty char literal (always rejected -- there's no ordinal
    // value for it to hold), an empty string is a perfectly ordinary,
    // zero-length C string.
    scpp::Program program = scpp::parse("int f(char* p) { p = \"\"; return 0; }");
    const scpp::Expr& assign = *program.functions[0].body->statements[0]->expr;
    expect(assign.rhs->kind == scpp::ExprKind::StringLiteral,
           "empty_string_literal_is_allowed: rhs should be a StringLiteral");
    expect(assign.rhs->name.empty(), "empty_string_literal_is_allowed: decoded content should be empty");
}

void test_unsupported_string_escape_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f(char* p) { p = \"\\z\"; return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "unsupported_string_escape_is_rejected: expected a ParseError");
}

void test_const_char_pointer_type() {
    // `const T*` (ch02 §2.1's realistic C signature compatibility -- e.g.
    // `const char* fmt`) parses as its own distinct Pointer type: scpp
    // now properly tracks pointer constness via is_mutable_pointee (ch05
    // §5.7, ch08 Q9), rather than silently dropping `const`.
    scpp::Program program = scpp::parse("extern \"C\" int puts(const char* s);");
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
    scpp::Program program = scpp::parse("extern \"C\" int f(int* p);");
    const scpp::Type& param_type = program.functions[0].params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer, "plain_pointer_defaults_to_mutable_pointee: should be Pointer");
    expect(param_type.is_mutable_pointee, "plain_pointer_defaults_to_mutable_pointee: is_mutable_pointee should be true");
}

// ch05/ch06: `const T name = expr;` -- a bare (non-reference,
// non-pointer) `const`-qualified local -- parses and sets Stmt::is_const,
// distinct from `const T&`/`const T*`'s own, separately-tracked
// read-only-ness (Type::is_mutable_ref/is_mutable_pointee).
void test_const_local_variable_parses() {
    scpp::Program program = scpp::parse("int f() { const int x = 5; return x; }");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "const_local_variable_parses: statement 0 should be VarDecl");
    expect(decl.is_const, "const_local_variable_parses: is_const should be true");
    expect(is_named_type(decl.type, "int"), "const_local_variable_parses: type should be plain 'int'");
}

// A plain (non-const) local must default to is_const == false -- the
// overwhelmingly common case, unaffected by the new tracking above.
void test_ordinary_local_variable_is_not_const() {
    scpp::Program program = scpp::parse("int f() { int x = 5; return x; }");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(!decl.is_const, "ordinary_local_variable_is_not_const: is_const should be false");
}

// ch05/ch06: a `const` local can never be given a value afterward, so
// omitting its initializer entirely (unlike an ordinary local, which may
// be declared bare) is rejected right at parse time.
void test_const_local_variable_without_initializer_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f() { const int x; return x; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "const_local_variable_without_initializer_is_rejected: expected a ParseError");
}

// Phase A: `constexpr` / `consteval` syntax is now represented directly in
// the AST so later constant-evaluation phases can consume it.
void test_constexpr_function_parses() {
    scpp::Program program = scpp::parse("constexpr int answer() { return 42; }\n");
    expect(program.functions[0].eval_mode == scpp::FunctionEvalMode::Constexpr,
           "constexpr_function_parses: eval_mode should be Constexpr");
}

void test_consteval_function_parses() {
    scpp::Program program = scpp::parse("consteval int answer() { return 42; }\n");
    expect(program.functions[0].eval_mode == scpp::FunctionEvalMode::Consteval,
           "consteval_function_parses: eval_mode should be Consteval");
}

void test_constexpr_constructor_parses() {
    scpp::Program program = scpp::parse("class Box { public: constexpr Box(int v) { value = v; } int value; };\n");
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
    scpp::Program program = scpp::parse("int f() { constexpr int x = 5; return x; }\n");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "constexpr_local_variable_parses: statement 0 should be VarDecl");
    expect(decl.is_constexpr, "constexpr_local_variable_parses: is_constexpr should be true");
    expect(!decl.is_const, "constexpr_local_variable_parses: is_const should stay false");
}

void test_constexpr_local_variable_without_initializer_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int f() { constexpr int x; return x; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "constexpr_local_variable_without_initializer_is_rejected: expected a ParseError");
}

void test_if_consteval_parses() {
    scpp::Program program = scpp::parse("int f() { if consteval { return 1; } else { return 2; } }\n");
    const scpp::Stmt& stmt = *program.functions[0].body->statements[0];
    expect(stmt.kind == scpp::StmtKind::If, "if_consteval_parses: statement 0 should be If");
    expect(stmt.if_mode == scpp::IfMode::ConstevalTrue, "if_consteval_parses: if_mode should be ConstevalTrue");
    expect(stmt.else_branch != nullptr, "if_consteval_parses: else branch should be present");
}

void test_if_not_consteval_parses() {
    scpp::Program program = scpp::parse("int f() { if !consteval { return 1; } else { return 2; } }\n");
    const scpp::Stmt& stmt = *program.functions[0].body->statements[0];
    expect(stmt.if_mode == scpp::IfMode::ConstevalFalse,
           "if_not_consteval_parses: if_mode should be ConstevalFalse");
}

// ch11 §11.3: `export module name;` marks a primary interface unit.
void test_export_module_declaration() {
    scpp::Program program = scpp::parse("export module std;\n");
    expect(program.module_name == "std", "export_module_declaration: module_name should be 'std'");
    expect(program.is_module_interface, "export_module_declaration: should be an interface unit");
    expect(!program.is_module_impl, "export_module_declaration: should not be an implementation unit");
}

// ch11 §11.3: a dotted module name (`org.lotx.cmath`) is read segment by
// segment (Identifier Dot Identifier ...), matching real module-name
// syntax, not namespace `::` syntax.
void test_dotted_module_name_declaration() {
    scpp::Program program = scpp::parse("export module org.lotx.cmath;\n");
    expect(program.module_name == "org.lotx.cmath", "dotted_module_name_declaration: expected 'org.lotx.cmath'");
}

// ch11 §11.3: `module name;` (no `export`) is an implementation unit.
void test_plain_module_declaration_is_implementation_unit() {
    scpp::Program program = scpp::parse("module std;\n");
    expect(program.module_name == "std", "plain_module_declaration_is_implementation_unit: module_name should be 'std'");
    expect(!program.is_module_interface,
           "plain_module_declaration_is_implementation_unit: should not be an interface unit");
    expect(program.is_module_impl, "plain_module_declaration_is_implementation_unit: should be an implementation unit");
}

// A file with no module declaration at all is unaffected -- module_name
// stays empty, matching every scpp file before this chapter.
void test_no_module_declaration_leaves_module_name_empty() {
    scpp::Program program = scpp::parse("int main() { return 0; }");
    expect(program.module_name.empty(), "no_module_declaration_leaves_module_name_empty: module_name should be empty");
    expect(!program.is_module_interface && !program.is_module_impl,
           "no_module_declaration_leaves_module_name_empty: neither interface nor impl unit");
}

// ch11 §11.4: `namespace std { ... }` qualifies every nested
// declaration's name with the namespace prefix, and records
// namespace_path separately.
void test_namespace_qualifies_struct_name() {
    scpp::Program program = scpp::parse("namespace std { struct Point { int x; }; }");
    expect(program.structs.size() == 1, "namespace_qualifies_struct_name: expected 1 struct");
    const scpp::StructDef& def = program.structs[0];
    expect(def.name == "std::Point", "namespace_qualifies_struct_name: name should be 'std::Point'");
    expect(def.namespace_path.size() == 1 && def.namespace_path[0] == "std",
           "namespace_qualifies_struct_name: namespace_path should be ['std']");
}

// ch11 §11.4: the C++17 one-line nested namespace form (`namespace
// a::b { ... }`) records every segment in namespace_path, in order.
void test_nested_namespace_one_liner_qualifies_function_name() {
    scpp::Program program = scpp::parse("namespace a::b { int f() { return 0; } }");
    expect(program.functions.size() == 1, "nested_namespace_one_liner: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.name == "a::b::f", "nested_namespace_one_liner: name should be 'a::b::f'");
    expect(fn.namespace_path.size() == 2 && fn.namespace_path[0] == "a" && fn.namespace_path[1] == "b",
           "nested_namespace_one_liner: namespace_path should be ['a', 'b']");
}

// A namespace-qualified type reference (`std::Point`) resolves once the
// declaration itself has already registered its fully-qualified name.
void test_qualified_type_reference_parses() {
    scpp::Program program =
        scpp::parse("namespace std { struct Point { int x; }; }\n"
                     "int use_it() { std::Point p; return p.x; }");
    expect(program.functions.size() == 1, "qualified_type_reference_parses: expected 1 function");
    const scpp::Stmt& decl = *program.functions[0].body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "qualified_type_reference_parses: expected a VarDecl");
    expect(is_named_type(decl.type, "std::Point"), "qualified_type_reference_parses: type should be 'std::Point'");
}

// ch11 §11.3: `export` prefixing a top-level function marks it exported.
void test_export_prefix_marks_function_exported() {
    scpp::Program program = scpp::parse("export module std;\nnamespace std { export int f() { return 0; } }");
    expect(program.functions.size() == 1, "export_prefix_marks_function_exported: expected 1 function");
    expect(program.functions[0].is_exported, "export_prefix_marks_function_exported: should be exported");
}

// A declaration with no `export` prefix defaults to not-exported, even
// inside a namespace.
void test_no_export_prefix_leaves_function_not_exported() {
    scpp::Program program = scpp::parse("namespace std { int f() { return 0; } }");
    expect(!program.functions[0].is_exported, "no_export_prefix_leaves_function_not_exported: should not be exported");
}

// ch11 §11.3: `export { ... }` groups several declarations under one
// export marker, equivalent to prefixing each individually.
void test_export_group_marks_multiple_declarations_exported() {
    scpp::Program program = scpp::parse(
        "export module std;\n"
        "namespace std { export { int f() { return 0; } int g() { return 1; } } }");
    expect(program.functions.size() == 2, "export_group_marks_multiple_declarations_exported: expected 2 functions");
    expect(program.functions[0].is_exported && program.functions[1].is_exported,
           "export_group_marks_multiple_declarations_exported: both should be exported");
}

// ch11 §11.3: `export class Name { ... };` exports the whole class --
// every synthesized method inherits is_exported, not just the class
// name entry itself.
void test_export_class_propagates_to_methods() {
    scpp::Program program = scpp::parse(
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

// ch11 §11.5: `export` on a declaration whose namespace doesn't match
// the module's own name is a compile error -- both gates (export
// marker, correct namespace) are independently mandatory.
void test_export_outside_matching_namespace_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("export module std;\nnamespace other { export int f() { return 0; } }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "export_outside_matching_namespace_is_rejected: expected a ParseError");
}

// ch11 §11.5: `export` with no enclosing namespace at all (while inside
// a module) is also rejected -- "lives in the required namespace" can't
// be satisfied by "no namespace".
void test_export_with_no_namespace_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("export module std;\nexport int f() { return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "export_with_no_namespace_is_rejected: expected a ParseError");
}

// ch11 §11.5: a namespace nested *deeper* than the module's own name is
// still fine (a prefix requirement, not exact-match).
void test_export_in_deeper_nested_namespace_is_allowed() {
    scpp::Program program = scpp::parse(
        "export module org.lotx.cmath;\n"
        "namespace org::lotx::cmath::trig { export int f() { return 0; } }");
    expect(program.functions.size() == 1, "export_in_deeper_nested_namespace_is_allowed: expected 1 function");
    expect(program.functions[0].is_exported, "export_in_deeper_nested_namespace_is_allowed: should be exported");
}

// ch11 §11.3: `export` on a declaration in a file with no module
// declaration at all has nothing to export from -- rejected.
void test_export_without_any_module_declaration_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("namespace std { export int f() { return 0; } }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "export_without_any_module_declaration_is_rejected: expected a ParseError");
}

// ch11 §11.6: a bare `extern` (no `"C"` string) declaration has ordinary
// scpp linkage -- a bodyless declaration distinct from `extern "C"`
// (is_extern_c stays false, is_module_extern is set instead).
void test_bare_extern_declaration_is_module_extern() {
    scpp::Program program = scpp::parse("extern int square(int x);");
    expect(program.functions.size() == 1, "bare_extern_declaration_is_module_extern: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_module_extern, "bare_extern_declaration_is_module_extern: is_module_extern should be true");
    expect(!fn.is_extern_c, "bare_extern_declaration_is_module_extern: is_extern_c should be false");
    expect(fn.body == nullptr, "bare_extern_declaration_is_module_extern: body should be null (bodyless declaration)");
}

// A bare `extern` declaration is namespace-qualified like any ordinary
// scpp-linkage declaration (unlike `extern "C"`, which never is).
void test_bare_extern_declaration_is_namespace_qualified() {
    scpp::Program program = scpp::parse("namespace org::lotx::cmath { extern int sqrt(int x); }");
    expect(program.functions.size() == 1, "bare_extern_declaration_is_namespace_qualified: expected 1 function");
    expect(program.functions[0].name == "org::lotx::cmath::sqrt",
           "bare_extern_declaration_is_namespace_qualified: expected qualified name");
}

// ch11 §11.4: `export module name:part;` declares an interface
// partition -- module_name stays just the base dotted name, with the
// part after ':' recorded separately in partition_name.
void test_partition_declaration_sets_partition_name() {
    scpp::Program program = scpp::parse("export module mylib.math:trig;\n");
    expect(program.module_name == "mylib.math", "partition_declaration_sets_partition_name: expected 'mylib.math'");
    expect(program.partition_name == "trig", "partition_declaration_sets_partition_name: expected 'trig'");
    expect(program.is_module_interface, "partition_declaration_sets_partition_name: should be an interface partition");
}

// ch11 §11.4: `module name:part;` (no `export`) declares an
// implementation partition.
void test_implementation_partition_declaration() {
    scpp::Program program = scpp::parse("module mylib.math:detail;\n");
    expect(program.module_name == "mylib.math", "implementation_partition_declaration: expected 'mylib.math'");
    expect(program.partition_name == "detail", "implementation_partition_declaration: expected 'detail'");
    expect(!program.is_module_interface, "implementation_partition_declaration: should not be an interface partition");
    expect(program.is_module_impl, "implementation_partition_declaration: should be an implementation partition");
}

// ch11 §11.4: `import :part;` inside a file with no module declaration
// of its own makes no sense -- partitions only exist within a module.
void test_partition_import_outside_module_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("import :trig;\nint main() { return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "partition_import_outside_module_is_rejected: expected a ParseError");
}

// ch11 §11.4: `import :part;` without a partition resolver configured
// (mirrors the existing cross-module "no module resolver" check) is
// rejected with a clear error rather than crashing.
void test_partition_import_without_resolver_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("export module mylib.math;\nimport :trig;\n");
    } catch (const scpp::ParseError&) {
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
        return scpp::parse(
            "export module mylib.math:trig;\n"
            "namespace mylib::math {\n"
            "    export int sin_deg_approx(int degrees) { return degrees / 2; }\n"
            "    int private_helper(int x) { return x; }\n"
            "}\n");
    };
    scpp::Program program = scpp::parse(
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
        return scpp::parse(
            "export module mylib.math:trig;\n"
            "namespace mylib::math { export int sin_deg_approx(int degrees) { return degrees / 2; } }\n");
    };
    scpp::Program program = scpp::parse(
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
        return scpp::parse("module mylib.math:detail;\nnamespace mylib::math { export int f() { return 0; } }\n");
    };
    bool threw = false;
    try {
        scpp::parse("export module mylib.math;\nexport import :detail;\n", /*resolver=*/{}, partition_resolver);
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "export_import_on_implementation_partition_is_rejected: expected a ParseError");
}

// ch03: `T&&` (rvalue reference) is parsed only in a function parameter's
// declared type -- `is_rvalue_ref` set, `is_mutable_ref` also true (an
// rvalue-reference parameter is always fully mutable/ownable inside the
// callee).
void test_rvalue_reference_parameter_parses() {
    scpp::Program program = scpp::parse("int take(int&& x) { return x; }");
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
    scpp::Program program = scpp::parse("int take(int& x, const int& y) { return x + y; }");
    const scpp::Function& fn = program.functions[0];
    expect(!fn.params[0].type.is_rvalue_ref, "ordinary_reference_parameter_is_not_rvalue_ref: 'int&' param");
    expect(!fn.params[1].type.is_rvalue_ref, "ordinary_reference_parameter_is_not_rvalue_ref: 'const int&' param");
}

// ch03: `const T&&` is rejected -- a moved-from value must be mutable to
// move *from*, so `const` can never qualify an rvalue reference.
void test_const_rvalue_reference_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int take(const int&& x) { return x; }");
    } catch (const scpp::ParseError&) {
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
        try {
            if (source.find("import std;") != std::string::npos) {
                parse_with_std_imports(source);
            } else {
                scpp::parse(source);
            }
        } catch (const scpp::ParseError&) {
            threw = true;
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
    expect_rejected("import std;\nint f() { std::unique_ptr<int&&> p; return 0; }", "unique_ptr element type");
}

// ch05 §5.11: `template<typename T> concept Name = requires(...) { ...
// };` with a *compound* requirement (`{ expr } -> std::same_as<T>;`)
// synthesizes a hidden witness class (ClassDef::is_concept_witness) with
// one bodyless method per requirement, named via the same
// `ClassName_memberName` scheme every other method uses -- so the
// return type and parameter (receiver) shape are exactly what an
// ordinary method's would be.
void test_concept_compound_requirement_synthesizes_witness_class() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
        "template<typename T>\n"
        "concept IntConsumer = requires(T f, int x) { f(x); };\n"
        "int main() { return 0; }\n");
    expect(program.concepts.size() == 1,
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: expected 1 concept");
    const scpp::ConceptDef& def = program.concepts[0];
    expect(def.requires_param_name == "f",
           "concept_simple_direct_invocation_requirement_synthesizes_call_method: requires_param_name should be 'f'");
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
            expect(fn.params[0].type.is_mutable_ref,
                   "concept_simple_direct_invocation_requirement_synthesizes_call_method: 'this' should be mutable "
                   "('T f' has no const)");
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
    try {
        scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) {\n"
            "    { other.area() } -> std::same_as<int>;\n"
            "};\n");
    } catch (const scpp::ParseError&) {
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
    try {
        scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) {\n"
            "    { t.area() } -> std::convertible_to<int>;\n"
            "};\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "concept_convertible_to_constraint_is_rejected: expected a ParseError");
}

// ch05 §5.11: a requirement's call argument must be a bare reference to
// one of the requires-expression's *other* (non-placeholder) parameters
// -- an unknown identifier is rejected.
void test_concept_requirement_unknown_argument_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("template<typename T>\nconcept IntConsumer = requires(T f, int x) { f(y); };\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "concept_requirement_unknown_argument_is_rejected: expected a ParseError");
}

// ch11 §11.5: `export` on a concept declaration, like every other
// top-level declaration, has no effect (is rejected) outside a module
// file.
void test_export_concept_outside_module_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("export template<typename T>\nconcept Shape = requires(const T& t) { t.area(); };\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "export_concept_outside_module_is_rejected: expected a ParseError");
}

// ch11 §11.4/§11.5: a concept declared inside `namespace a { ... }`
// namespace-qualifies its own name and its witness class/method exactly
// like a struct/class/function would.
void test_concept_inside_namespace_is_qualified() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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

// ch05 §5.11: `ConceptName auto&&` (a move-in generic parameter,
// e.g. for passing a closure) parses with is_rvalue_ref set, mirroring
// an ordinary `T&&` parameter exactly, just with a concept's witness
// class standing in for a concrete type.
void test_generic_parameter_auto_rvalue_ref_parses() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse("int identity(auto x) { return 0; }\n");
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
    scpp::Program program = scpp::parse("int main() { return [](auto x) { return 0; }(1); }\n");
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
    try {
        scpp::parse("int f(NotAConcept auto& x) { return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "generic_parameter_unknown_concept_is_rejected: expected a ParseError");
}

// ch05 §5.11: a generic (concept-constrained) parameter is only
// supported on a free function in this version -- rejected on a method
// or constructor.
void test_generic_parameter_on_method_is_rejected() {
    bool threw = false;
    try {
        scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) { t.area(); };\n"
            "class Widget {\n"
            "public:\n"
            "    Widget() { return; }\n"
            "    int touch(Shape auto& s) { return 0; }\n"
            "};\n");
    } catch (const scpp::ParseError&) {
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
    scpp::Program program = scpp::parse(
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
    scpp::Program value_program = scpp::parse(
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

    scpp::Program ref_program = scpp::parse(
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
    scpp::Program mixed_program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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

    scpp::Program star_program = scpp::parse(
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
    scpp::Program program = scpp::parse(
        "struct Op { int (*fn)(int, int); };\n"
        "int add(int a, int b) { return a + b; }\n"
        "int main() {\n"
        "    int (*fp)(int, int) = add;\n"
        "    int (* [[scpp::unsafe]] up)(int, int) = add;\n"
        "    Op op;\n"
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
    try {
        scpp::parse(
            "template<typename T>\n"
            "concept Shape = requires(const T& t) { t.area(); };\n"
            "int apply(int x) { return x; }\n"
            "int main() {\n"
            "    apply([](Shape auto& s) { return 0; });\n"
            "    return 0;\n"
            "}\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "lambda_generic_parameter_is_rejected: expected a ParseError");
}

// ch05 §5.12: `mutable` parses and sets lambda_is_mutable.
void test_lambda_mutable_keyword_parses() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
        "template<typename Sig>\n"
        "class Holder {\n"
        "public:\n"
        "    int value;\n"
        "};\n"
        "int main() { Holder<int(int, int)> h; return 0; }\n");
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
    scpp::Program program = scpp::parse(
        "template<typename Sig>\n"
        "class Holder {\n"
        "public:\n"
        "    int value;\n"
        "};\n"
        "int main() { Holder<int(int) const &&> h; return 0; }\n");
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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

void test_class_partial_specialization_on_function_type_parses() {
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse("template<typename... Ts> class Box;\n"
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse(
            "template<typename T>\n"
            "struct Pair {\n"
            "    T x;\n"
            "};\n"
            "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "generic_struct_bare_type_param_is_rejected: expected a ParseError");
}

// ch05 §5.14: `Name<Arg>` (a generic-type instantiation) parses to a
// Type whose `name` still names the *template* and whose `template_args`
// holds the (single, v0.1-only) concrete argument -- left unresolved
// for movecheck's Monomorphizer (see Type::template_args' own comment).
void test_generic_type_instantiation_parses_with_template_args() {
    scpp::Program program = scpp::parse(
        "template<typename T>\n"
        "class Vec {\n"
        "    T item;\n"
        "};\n"
        "int main() {\n"
        "    Vec<int> v;\n"
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

// ch05 §5.14: `class Derived : public Base { ... };` records
// base_class_name/base_access on the ClassDef.
void test_class_public_inheritance_parses() {
    scpp::Program program = scpp::parse(
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
    expect(dog->base_class_name == "Animal", "class_public_inheritance_parses: base_class_name should be 'Animal'");
    expect(dog->base_access == scpp::AccessSpecifier::Public,
           "class_public_inheritance_parses: base_access should be Public");
}

// ch05 §5.14: `class Derived : Base { ... };` (no access keyword)
// defaults to private inheritance, matching real C++'s own default for
// `class` (unlike `struct`, which defaults to public -- but structs
// have no inheritance here at all).
void test_class_inheritance_defaults_to_private() {
    scpp::Program program = scpp::parse(
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
    expect(dog->base_access == scpp::AccessSpecifier::Private,
           "class_inheritance_defaults_to_private: base_access should default to Private");
}

// ch05 §5.14: a base class must already be a declared class (single-
// pass parsing) -- referencing an undeclared name is a ParseError.
void test_class_inheritance_from_undeclared_class_is_rejected() {
    bool threw = false;
    try {
        scpp::parse(
            "class Dog : public Animal {\n"
            "public:\n"
            "    Dog() { return; }\n"
            "};\n"
            "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "class_inheritance_from_undeclared_class_is_rejected: expected a ParseError");
}

// ch05 §5.14: `template<typename... Ts> class Tuple;` -- a variadic
// primary template's own bodyless forward declaration -- marks the
// ClassDef is_variadic_primary_template, records its single pack
// parameter, and pushes no fields/base at all.
void test_variadic_primary_template_decl_parses() {
    scpp::Program program = scpp::parse("template<typename... Ts> class Tuple;\n"
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
    scpp::Program program = scpp::parse("template<typename... Ts> class Tuple;\n"
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
// Tail(is_pack)), base_class_name/base_pack_arg_name (the pack spread
// as the base's sole argument), and the field typed by the head
// parameter.
void test_variadic_recursive_specialization_parses() {
    scpp::Program program = scpp::parse("template<typename... Ts> class Tuple;\n"
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
    expect(recursive_case->base_class_name == "Tuple",
           "variadic_recursive_specialization_parses: base_class_name should be 'Tuple'");
    expect(recursive_case->base_pack_arg_name == "Tail",
           "variadic_recursive_specialization_parses: base_pack_arg_name should be 'Tail'");
    expect(recursive_case->fields.size() == 1 && recursive_case->fields[0].type.name == "Head",
           "variadic_recursive_specialization_parses: expected a single field typed 'Head'");
}

// ch05 §5.14: a use-site instantiation of a variadic generic type,
// `Tuple<int, bool, char>`, parses with all 3 concrete arguments
// recorded (in order) on Type::template_args.
void test_variadic_instantiation_with_multiple_args_parses() {
    scpp::Program program = scpp::parse("template<typename... Ts> class Tuple;\n"
                                         "template<> class Tuple<> {};\n"
                                         "template<typename Head, typename... Tail>\n"
                                         "class Tuple<Head, Tail...> : private Tuple<Tail...> {\n"
                                         "    Head head;\n"
                                         "};\n"
                                         "int main() {\n"
                                         "    Tuple<int, bool, char> t;\n"
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
    try {
        scpp::parse("template<typename... Ts> struct Tuple;\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "variadic_struct_is_rejected: expected a ParseError");
}

// ch05 §5.14: a specialization referencing an undeclared primary
// template is a ParseError.
void test_variadic_specialization_without_primary_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("template<> class Tuple<> {};\n"
                    "int main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "variadic_specialization_without_primary_is_rejected: expected a ParseError");
}

// ch05 §5.11: `template<typename T> T name(params) { body }` -- the full
// header form for a generic function (as opposed to the abbreviated
// `Concept auto` form) -- records Function::template_params and marks
// is_generic_template.
void test_generic_function_full_header_form_parses() {
    scpp::Program program = scpp::parse("template<typename T> T make() {\n"
                                         "    T x;\n"
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
    scpp::Program program = scpp::parse("template<typename T, typename U> void f(T a, U b) { return; }\n"
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
    scpp::Program program = scpp::parse("template<typename T> concept HasGet = requires(T t) { t.get(); };\n"
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
    scpp::Program program = scpp::parse("class Circle {\n"
                                         "public:\n"
                                         "    Circle() { return; }\n"
                                         "};\n"
                                         "template<typename T> T make() {\n"
                                         "    T x;\n"
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
    scpp::Program program = scpp::parse("template<typename T> int pick(T x, int y) {\n"
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
    scpp::Program program = scpp::parse("template<int Idx, typename... Ts> class TupleImpl;\n"
                                         "template<int Idx> class TupleImpl<Idx> {};\n"
                                         "template<int Idx, typename Head, typename... Tail>\n"
                                         "class TupleImpl<Idx, Head, Tail...> : public TupleImpl<Idx + 1, Tail...> {\n"
                                         "public:\n"
                                         "    Head value;\n"
                                         "};\n"
                                         "template<int I, typename Head, typename... Tail>\n"
                                         "Head& get(TupleImpl<I, Head, Tail...>& t) { return t.value; }\n"
                                         "int main() {\n"
                                         "    TupleImpl<0, int, bool> t;\n"
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
// and the recursive case's own base clause records base_non_type_arg
// (the "Idx + 1" expression) alongside base_pack_arg_name ("Tail").
void test_variadic_specialization_with_leading_non_type_param_parses() {
    scpp::Program program = scpp::parse("template<int Idx, typename... Ts> class TupleImpl;\n"
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
    expect(recursive_case->base_class_name == "TupleImpl" && recursive_case->base_non_type_arg != nullptr &&
               recursive_case->base_pack_arg_name == "Tail",
           "variadic_specialization_with_leading_non_type_param_parses: expected base_class_name='TupleImpl', "
           "base_non_type_arg set, base_pack_arg_name='Tail'");
}

void test_namespace_relative_qualified_generic_type_declaration_parses() {
    scpp::Program program = scpp::parse(
        "namespace m {\n"
        "namespace detail {\n"
        "template<typename Left, typename Right>\n"
        "class Pair {\n"
        "public:\n"
        "    Pair() { return; }\n"
        "};\n"
        "}\n"
        "int f() {\n"
        "    detail::Pair<int, bool> pair;\n"
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

void test_break_and_continue_parse_inside_loop() {
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("int main() { break; return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "break_outside_loop_is_rejected: expected a ParseError");
}

void test_continue_outside_loop_is_rejected() {
    bool threw = false;
    try {
        scpp::parse("int main() { continue; return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "continue_outside_loop_is_rejected: expected a ParseError");
}

void test_conditional_expression_parses() {
    scpp::Program program = scpp::parse("int main() { return true ? 1 : 2; }\n");
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
    scpp::Program program = scpp::parse(
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
    scpp::Program program = scpp::parse(
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
    try {
        scpp::parse("enum Color { red, green };\nint main() { return 0; }\n");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "old_style_enum_is_rejected: expected a ParseError");
}

} // namespace

int main() {
    test_int_main_return();
    test_function_with_params();
    test_var_decl_and_if_else();
    test_while_loop();
    test_break_and_continue_parse_inside_loop();
    test_break_outside_loop_is_rejected();
    test_continue_outside_loop_is_rejected();
    test_unsafe_block_sets_is_unsafe_flag();
    test_ordinary_block_is_not_unsafe();
    test_nested_unsafe_blocks_parse();
    test_enum_class_declaration_parses();
    test_enum_class_underlying_type_parses();
    test_old_style_enum_is_rejected();
    test_bare_unsafe_identifier_followed_by_return_is_parse_error();
    test_unsafe_attribute_on_non_block_statement_has_no_effect();
    test_function_level_unsafe_marker_parses();
    test_unsafe_attribute_on_struct_is_rejected();
    test_thread_safety_attribute_on_struct_parses();
    test_thread_safety_attributes_on_class_parse();
    test_thread_safety_attribute_on_parameter_parses();
    test_user_declared_move_constructor_is_rejected();
    test_constructor_taking_other_type_rvalue_reference_parses();
    test_operator_assign_parses();
    test_user_declared_move_assignment_operator_is_rejected();
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
    test_arrow_desugars_to_member_of_deref();
    test_chained_arrow_and_dot();
    test_multiplication_is_not_confused_with_dereference();
    test_parenthesized_expression();
    test_parse_error_on_missing_semicolon();
    test_struct_declaration();
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
    test_span_of_const_element_type();
    test_storage_for_type_declaration();
    test_move_expression();
    test_move_as_function_argument();
    test_make_unique_zero_args();
    test_make_unique_with_arg();
    test_make_unique_of_struct_type();
    test_new_and_delete_parse();
    test_placement_new_parse();
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
    test_no_module_declaration_leaves_module_name_empty();
    test_namespace_qualifies_struct_name();
    test_nested_namespace_one_liner_qualifies_function_name();
    test_qualified_type_reference_parses();
    test_export_prefix_marks_function_exported();
    test_no_export_prefix_leaves_function_not_exported();
    test_export_group_marks_multiple_declarations_exported();
    test_export_class_propagates_to_methods();
    test_export_outside_matching_namespace_is_rejected();
    test_export_with_no_namespace_is_rejected();
    test_export_in_deeper_nested_namespace_is_allowed();
    test_export_without_any_module_declaration_is_rejected();
    test_bare_extern_declaration_is_module_extern();
    test_bare_extern_declaration_is_namespace_qualified();
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
    test_class_partial_specialization_on_function_type_parses();
    test_generic_class_method_requires_clause_parses();
    test_generic_struct_concept_constrained_type_param_parses();
    test_generic_struct_bare_type_param_is_rejected();
    test_generic_type_instantiation_parses_with_template_args();
    test_class_public_inheritance_parses();
    test_class_inheritance_defaults_to_private();
    test_class_inheritance_from_undeclared_class_is_rejected();
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
    test_explicit_template_argument_call_with_multiple_value_args_parses();
    test_explicit_non_type_template_argument_call_parses();
    test_variadic_specialization_with_leading_non_type_param_parses();
    test_namespace_relative_qualified_generic_type_declaration_parses();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All parser tests passed.\n";
    return 0;
}
