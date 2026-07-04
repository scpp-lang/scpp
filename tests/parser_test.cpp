import scpp.parser;
import scpp.ast;

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

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

void test_int_main_return() {
    scpp::Program program = scpp::parse("int main() { return 42; }");
    expect(program.functions.size() == 1, "int_main_return: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(!fn.is_safe, "int_main_return: function should not be safe");
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

void test_safe_function_with_params() {
    scpp::Program program = scpp::parse("safe int add(int a, int b) { return a + b; }");
    expect(program.functions.size() == 1, "safe_function_with_params: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_safe, "safe_function_with_params: function should be safe");
    expect(fn.params.size() == 2, "safe_function_with_params: expected 2 params");
    expect(is_named_type(fn.params[0].type, "int") && fn.params[0].name == "a",
           "safe_function_with_params: param 0 should be 'int a'");
    expect(is_named_type(fn.params[1].type, "int") && fn.params[1].name == "b",
           "safe_function_with_params: param 1 should be 'int b'");

    const scpp::Stmt& ret = *fn.body->statements[0];
    expect(ret.expr->kind == scpp::ExprKind::Binary, "safe_function_with_params: expr should be Binary");
    expect(ret.expr->binary_op == scpp::BinaryOp::Add, "safe_function_with_params: op should be Add");
    expect(ret.expr->lhs->kind == scpp::ExprKind::Identifier && ret.expr->lhs->name == "a",
           "safe_function_with_params: lhs should be identifier 'a'");
    expect(ret.expr->rhs->kind == scpp::ExprKind::Identifier && ret.expr->rhs->name == "b",
           "safe_function_with_params: rhs should be identifier 'b'");
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
    // `unsafe { }` (ch01 §1.3) is an ordinary Block statement with
    // is_unsafe set -- see parse_unsafe_block.
    scpp::Program program = scpp::parse("int f() { unsafe { int x = 1; } return 0; }");
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
    // Sanity check for the flag's default: a plain `{ }` (no `unsafe`
    // keyword) must never be mistaken for an unsafe block.
    scpp::Program program = scpp::parse("int f() { { int x = 1; } return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& plain_block = *fn.body->statements[0];
    expect(plain_block.kind == scpp::StmtKind::Block, "ordinary_block_is_not_unsafe: should be a Block");
    expect(!plain_block.is_unsafe, "ordinary_block_is_not_unsafe: is_unsafe should be false");
}

void test_nested_unsafe_blocks_parse() {
    // `unsafe { unsafe { ... } }` (ch01 §1.3's nesting rule) -- both
    // levels independently set is_unsafe.
    scpp::Program program = scpp::parse("int f() { unsafe { unsafe { int x = 1; } } return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& outer = *fn.body->statements[0];
    expect(outer.is_unsafe, "nested_unsafe_blocks_parse: outer block should be unsafe");
    expect(outer.statements.size() == 1, "nested_unsafe_blocks_parse: outer block should have 1 statement");
    const scpp::Stmt& inner = *outer.statements[0];
    expect(inner.kind == scpp::StmtKind::Block, "nested_unsafe_blocks_parse: inner statement should be a Block");
    expect(inner.is_unsafe, "nested_unsafe_blocks_parse: inner block should also be unsafe");
}

void test_unsafe_without_brace_is_parse_error() {
    // v0.1 only supports the block-statement form: `unsafe` must always
    // be followed by `{`, never a single bare statement.
    bool threw = false;
    try {
        scpp::parse("int f() { unsafe return 1; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "unsafe_without_brace_is_parse_error: expected a ParseError to be thrown");
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
    expect(def.fields.size() == 2, "struct_declaration: expected 2 fields");
    expect(is_named_type(def.fields[0].type, "int") && def.fields[0].name == "x",
           "struct_declaration: field 0 should be 'int x'");
    expect(is_named_type(def.fields[1].type, "int") && def.fields[1].name == "y",
           "struct_declaration: field 1 should be 'int y'");
    expect(program.functions.size() == 1, "struct_declaration: expected 1 function after the struct");
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
    scpp::Program program = scpp::parse("int f() { std::unique_ptr<int> a; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.kind == scpp::StmtKind::VarDecl, "unique_ptr_type_declaration: statement 0 should be VarDecl");
    expect(decl.type.kind == scpp::TypeKind::UniquePtr,
           "unique_ptr_type_declaration: type should be UniquePtr");
    expect(decl.type.pointee != nullptr && is_named_type(*decl.type.pointee, "int"),
           "unique_ptr_type_declaration: pointee should be 'int'");
    expect(decl.var_name == "a", "unique_ptr_type_declaration: variable name should be 'a'");
    expect(decl.init == nullptr, "unique_ptr_type_declaration: no initializer given");
}

void test_unique_ptr_of_struct_type() {
    scpp::Program program = scpp::parse(
        "struct Point { int x; int y; };"
        "int f() { std::unique_ptr<Point> a; return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.type.kind == scpp::TypeKind::UniquePtr,
           "unique_ptr_of_struct_type: type should be UniquePtr");
    expect(decl.type.pointee != nullptr && is_named_type(*decl.type.pointee, "Point"),
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

void test_move_expression() {
    scpp::Program program = scpp::parse(
        "int f() {"
        "    std::unique_ptr<int> a;"
        "    std::unique_ptr<int> b = std::move(a);"
        "    return 0;"
        "}");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[1];
    expect(decl.kind == scpp::StmtKind::VarDecl, "move_expression: statement 1 should be VarDecl");
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::Move,
           "move_expression: initializer should be a Move expression");
    expect(decl.init->lhs->kind == scpp::ExprKind::Identifier && decl.init->lhs->name == "a",
           "move_expression: moved expression should be identifier 'a'");
}

void test_move_as_function_argument() {
    scpp::Program program = scpp::parse(
        "int consume(std::unique_ptr<int> p) { return 0; }"
        "int f() {"
        "    std::unique_ptr<int> a;"
        "    return consume(std::move(a));"
        "}");
    const scpp::Function& consume_fn = program.functions[0];
    expect(consume_fn.params.size() == 1 && consume_fn.params[0].type.kind == scpp::TypeKind::UniquePtr,
           "move_as_function_argument: 'consume' should take a UniquePtr param");

    const scpp::Function& f_fn = program.functions[1];
    const scpp::Stmt& ret = *f_fn.body->statements[1];
    expect(ret.expr->kind == scpp::ExprKind::Call && ret.expr->name == "consume",
           "move_as_function_argument: return expr should be a call to 'consume'");
    expect(ret.expr->args.size() == 1 && ret.expr->args[0]->kind == scpp::ExprKind::Move,
           "move_as_function_argument: call argument should be a Move expression");
}

void test_make_unique_zero_args() {
    scpp::Program program = scpp::parse("int f() { std::unique_ptr<int> a = std::make_unique<int>(); return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::MakeUnique,
           "make_unique_zero_args: initializer should be a MakeUnique expression");
    expect(is_named_type(decl.init->type, "int"), "make_unique_zero_args: element type should be 'int'");
    expect(decl.init->args.empty(), "make_unique_zero_args: expected 0 arguments");
}

void test_make_unique_with_arg() {
    scpp::Program program = scpp::parse("int f() { std::unique_ptr<int> a = std::make_unique<int>(42); return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::MakeUnique,
           "make_unique_with_arg: initializer should be a MakeUnique expression");
    expect(decl.init->args.size() == 1 && decl.init->args[0]->kind == scpp::ExprKind::IntegerLiteral &&
               decl.init->args[0]->int_value == 42,
           "make_unique_with_arg: expected a single IntegerLiteral 42 argument");
}

void test_make_unique_of_struct_type() {
    scpp::Program program = scpp::parse(
        "struct Point { int x; int y; };"
        "int f() { std::unique_ptr<Point> a = std::make_unique<Point>(); return 0; }");
    const scpp::Function& fn = program.functions[0];
    const scpp::Stmt& decl = *fn.body->statements[0];
    expect(decl.init != nullptr && decl.init->kind == scpp::ExprKind::MakeUnique,
           "make_unique_of_struct_type: initializer should be a MakeUnique expression");
    expect(is_named_type(decl.init->type, "Point"), "make_unique_of_struct_type: element type should be 'Point'");
}

void test_extern_c_single_declaration() {
    // ch02 §2.1: a bodyless `extern "C"` declaration.
    scpp::Program program = scpp::parse("extern \"C\" int c_abs(int n); int main() { return 0; }");
    expect(program.functions.size() == 2, "extern_c_single_declaration: expected 2 functions");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_extern_c, "extern_c_single_declaration: is_extern_c should be true");
    expect(!fn.is_safe, "extern_c_single_declaration: is_safe should be false");
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

void test_safe_extern_c_definition_is_allowed() {
    // ch02 §2.1: `safe` and `extern "C"` are orthogonal for a definition.
    scpp::Program program = scpp::parse("safe extern \"C\" int add(int a, int b) { return a + b; }");
    const scpp::Function& fn = program.functions[0];
    expect(fn.is_safe && fn.is_extern_c, "safe_extern_c_definition_is_allowed: both flags should be true");
    expect(fn.body != nullptr, "safe_extern_c_definition_is_allowed: body should be present");
}

void test_safe_extern_c_declaration_is_rejected() {
    // A bodyless declaration can never be `safe`: the compiler can't
    // verify an implementation it doesn't see (ch02 §2.1).
    bool threw = false;
    try {
        scpp::parse("safe extern \"C\" int foo(int x);");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "safe_extern_c_declaration_is_rejected: expected a ParseError");
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

void test_safe_prefix_on_extern_c_block_is_rejected() {
    // `safe` doesn't combine with the block form -- there's no single
    // item for it to attach to (ch02 §2.1); mark individual items `safe`
    // inside the block instead.
    bool threw = false;
    try {
        scpp::parse("safe extern \"C\" { int f(int x); }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "safe_prefix_on_extern_c_block_is_rejected: expected a ParseError");
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

void test_const_char_pointer_type() {
    // `const T*` (ch02 §2.1's realistic C signature compatibility -- e.g.
    // `const char* fmt`) parses as a plain Pointer type; scpp doesn't
    // track pointer constness, so the `const` is accepted but dropped.
    scpp::Program program = scpp::parse("extern \"C\" int puts(const char* s);");
    const scpp::Function& fn = program.functions[0];
    expect(fn.params.size() == 1, "const_char_pointer_type: expected 1 parameter");
    const scpp::Type& param_type = fn.params[0].type;
    expect(param_type.kind == scpp::TypeKind::Pointer, "const_char_pointer_type: parameter should be Pointer");
    expect(param_type.pointee != nullptr && is_named_type(*param_type.pointee, "char"),
           "const_char_pointer_type: pointee should be 'char'");
}

} // namespace

int main() {
    test_int_main_return();
    test_safe_function_with_params();
    test_var_decl_and_if_else();
    test_while_loop();
    test_unsafe_block_sets_is_unsafe_flag();
    test_ordinary_block_is_not_unsafe();
    test_nested_unsafe_blocks_parse();
    test_unsafe_without_brace_is_parse_error();
    test_operator_precedence();
    test_unary_and_call();
    test_dereference_expression();
    test_arrow_desugars_to_member_of_deref();
    test_chained_arrow_and_dot();
    test_multiplication_is_not_confused_with_dereference();
    test_parenthesized_expression();
    test_parse_error_on_missing_semicolon();
    test_struct_declaration();
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
    test_move_expression();
    test_move_as_function_argument();
    test_make_unique_zero_args();
    test_make_unique_with_arg();
    test_make_unique_of_struct_type();
    test_extern_c_single_declaration();
    test_extern_c_block_form();
    test_safe_extern_c_definition_is_allowed();
    test_safe_extern_c_declaration_is_rejected();
    test_extern_cpp_linkage_is_rejected();
    test_extern_c_varargs_declaration();
    test_varargs_on_definition_is_rejected();
    test_varargs_on_non_extern_function_is_rejected();
    test_void_return_and_void_pointer_types();
    test_safe_prefix_on_extern_c_block_is_rejected();
    test_char_type_declaration();
    test_char_literal_expression();
    test_char_literal_escape_sequences_decode_correctly();
    test_empty_char_literal_is_rejected();
    test_multi_character_char_literal_is_rejected();
    test_unsupported_char_escape_is_rejected();
    test_const_char_pointer_type();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All parser tests passed.\n";
    return 0;
}
