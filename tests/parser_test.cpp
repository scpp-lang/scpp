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

void test_int_main_return() {
    scpp::Program program = scpp::parse("int main() { return 42; }");
    expect(program.functions.size() == 1, "int_main_return: expected 1 function");
    const scpp::Function& fn = program.functions[0];
    expect(!fn.is_safe, "int_main_return: function should not be safe");
    expect(fn.return_type == "int", "int_main_return: return type should be 'int'");
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
    expect(fn.params[0].type_name == "int" && fn.params[0].name == "a",
           "safe_function_with_params: param 0 should be 'int a'");
    expect(fn.params[1].type_name == "int" && fn.params[1].name == "b",
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
    expect(decl.type_name == "int" && decl.var_name == "x",
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

} // namespace

int main() {
    test_int_main_return();
    test_safe_function_with_params();
    test_var_decl_and_if_else();
    test_while_loop();
    test_operator_precedence();
    test_unary_and_call();
    test_parenthesized_expression();
    test_parse_error_on_missing_semicolon();

    if (failures > 0) {
        std::cerr << failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "All parser tests passed.\n";
    return 0;
}
