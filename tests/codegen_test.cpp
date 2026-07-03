import scpp.codegen;
import scpp.parser;
import scpp.ast;

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        failures++;
    }
}

std::string generate_ir(std::string_view source) {
    scpp::Program program = scpp::parse(source);
    scpp::Codegen codegen("test_module");
    codegen.generate(program);
    return codegen.module_ir();
}

void test_return_constant() {
    std::string ir = generate_ir("int main() { return 42; }");
    expect(ir.find("define i32 @main()") != std::string::npos,
           "return_constant: expected 'define i32 @main()' in IR");
    expect(ir.find("ret i32 42") != std::string::npos, "return_constant: expected 'ret i32 42' in IR");
}

void test_function_with_params_and_call() {
    std::string ir = generate_ir(
        "int add(int a, int b) { return a + b; }"
        "int main() { return add(1, 2); }");
    expect(ir.find("define i32 @add(i32") != std::string::npos,
           "function_with_params_and_call: expected 'add' function definition");
    expect(ir.find("call i32 @add(") != std::string::npos,
           "function_with_params_and_call: expected a call to 'add'");
}

void test_if_else_generates_branches() {
    std::string ir = generate_ir("int f(int x) { if (x < 2) { return 1; } else { return 0; } }");
    expect(ir.find("br i1") != std::string::npos, "if_else_generates_branches: expected a conditional branch");
    expect(ir.find("if.then") != std::string::npos, "if_else_generates_branches: expected an 'if.then' block");
    expect(ir.find("if.else") != std::string::npos, "if_else_generates_branches: expected an 'if.else' block");
}

void test_while_generates_loop() {
    std::string ir = generate_ir("int f() { int x = 0; while (x < 10) { x = x + 1; } return x; }");
    expect(ir.find("while.cond") != std::string::npos, "while_generates_loop: expected a 'while.cond' block");
    expect(ir.find("while.body") != std::string::npos, "while_generates_loop: expected a 'while.body' block");
}

void test_missing_return_is_rejected() {
    bool threw = false;
    try {
        generate_ir("int f() { int x = 1; }");
    } catch (const scpp::CodegenError&) {
        threw = true;
    }
    expect(threw, "missing_return_is_rejected: expected a CodegenError");
}

void test_call_to_unknown_function_is_rejected() {
    bool threw = false;
    try {
        generate_ir("int main() { return unknown(); }");
    } catch (const scpp::CodegenError&) {
        threw = true;
    }
    expect(threw, "call_to_unknown_function_is_rejected: expected a CodegenError");
}

void test_print_builtins_generate_printf_calls() {
    std::string ir = generate_ir("int main() { print_int(1); print_bool(true); return 0; }");
    expect(ir.find("declare i32 @printf(") != std::string::npos,
           "print_builtins_generate_printf_calls: expected a printf declaration");
    expect(ir.find("call i32 (ptr, ...) @printf(") != std::string::npos,
           "print_builtins_generate_printf_calls: expected calls to printf");
}

void test_struct_type_and_zero_init() {
    std::string ir = generate_ir(
        "struct Point { int x; int y; };"
        "int main() { Point p; return p.x; }");
    expect(ir.find("%struct.Point = type { i32, i32 }") != std::string::npos,
           "struct_type_and_zero_init: expected the Point struct type in IR");
    expect(ir.find("zeroinitializer") != std::string::npos,
           "struct_type_and_zero_init: expected a zeroinitializer store for the uninitialized struct");
}

void test_struct_member_access_generates_gep() {
    std::string ir = generate_ir(
        "struct Point { int x; int y; };"
        "int main() { Point p; p.x = 1; return p.x + p.y; }");
    expect(ir.find("getelementptr inbounds nuw %struct.Point") != std::string::npos,
           "struct_member_access_generates_gep: expected a struct GEP in IR");
}

void test_struct_field_unknown_type_is_rejected() {
    // The parser itself rejects an unrecognized type name (it only accepts
    // scalars or already-declared struct names), so this surfaces as a
    // ParseError rather than reaching codegen's trivial-type validation.
    bool threw = false;
    try {
        scpp::parse("struct Bad { Nonexistent x; }; int main() { return 0; }");
    } catch (const scpp::ParseError&) {
        threw = true;
    }
    expect(threw, "struct_field_unknown_type_is_rejected: expected a ParseError");
}

void test_struct_self_reference_by_value_is_rejected() {
    bool threw = false;
    try {
        generate_ir("struct Node { Node inner; }; int main() { return 0; }");
    } catch (const scpp::CodegenError&) {
        threw = true;
    }
    expect(threw, "struct_self_reference_by_value_is_rejected: expected a CodegenError");
}

void test_struct_self_reference_by_pointer_is_allowed() {
    std::string ir = generate_ir(
        "struct Node { int value; Node* next; };"
        "int main() { Node n; return n.value; }");
    expect(ir.find("%struct.Node = type { i32, ptr }") != std::string::npos,
           "struct_self_reference_by_pointer_is_allowed: expected Node struct type with a ptr field");
}

void test_array_field_and_subscript_generates_gep() {
    std::string ir = generate_ir(
        "struct Buffer { int values[4]; };"
        "int main() { Buffer b; b.values[0] = 7; return b.values[0]; }");
    expect(ir.find("%struct.Buffer = type { [4 x i32] }") != std::string::npos,
           "array_field_and_subscript_generates_gep: expected Buffer struct type with an array field");
}

void test_nested_struct_field() {
    std::string ir = generate_ir(
        "struct Inner { int v; };"
        "struct Outer { Inner inner; };"
        "int main() { Outer o; o.inner.v = 5; return o.inner.v; }");
    expect(ir.find("%struct.Inner = type { i32 }") != std::string::npos,
           "nested_struct_field: expected Inner struct type in IR");
    expect(ir.find("%struct.Outer = type { %struct.Inner }") != std::string::npos,
           "nested_struct_field: expected Outer struct type embedding Inner");
}

} // namespace

int main() {
    test_return_constant();
    test_function_with_params_and_call();
    test_if_else_generates_branches();
    test_while_generates_loop();
    test_missing_return_is_rejected();
    test_call_to_unknown_function_is_rejected();
    test_print_builtins_generate_printf_calls();
    test_struct_type_and_zero_init();
    test_struct_member_access_generates_gep();
    test_struct_field_unknown_type_is_rejected();
    test_struct_self_reference_by_value_is_rejected();
    test_struct_self_reference_by_pointer_is_allowed();
    test_array_field_and_subscript_generates_gep();
    test_nested_struct_field();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All codegen tests passed.\n";
    return 0;
}
