module;

#include <memory>
#include <string>
#include <vector>

export module scpp.ast;

export namespace scpp {

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind {
    IntegerLiteral,
    BoolLiteral,
    Identifier,
    Binary,
    Unary,
    Call,
};

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    And,
    Or,
    Assign,
};

enum class UnaryOp {
    Neg,
    Not,
};

// A single expression node. Only the fields relevant to `kind` are populated;
// this keeps the AST as a flat, easy-to-pattern-match tagged union without
// needing a class hierarchy for the minimal M1 subset.
struct Expr {
    ExprKind kind;

    // IntegerLiteral
    long long int_value = 0;

    // BoolLiteral
    bool bool_value = false;

    // Identifier / Call (callee name)
    std::string name;

    // Binary
    BinaryOp binary_op{};
    ExprPtr lhs;
    ExprPtr rhs;

    // Unary (operand stored in `lhs`)
    UnaryOp unary_op{};

    // Call arguments
    std::vector<ExprPtr> args;
};

enum class StmtKind {
    VarDecl,
    Return,
    If,
    While,
    ExprStmt,
    Block,
};

struct Stmt {
    StmtKind kind;

    // VarDecl
    std::string type_name;
    std::string var_name;
    ExprPtr init; // optional

    // Return / ExprStmt (value/expr)
    ExprPtr expr;

    // If / While
    ExprPtr condition;
    StmtPtr then_branch;
    StmtPtr else_branch; // optional, If only

    // Block
    std::vector<StmtPtr> statements;
};

struct Param {
    std::string type_name;
    std::string name;
};

struct Function {
    bool is_safe = false;
    std::string return_type;
    std::string name;
    std::vector<Param> params;
    StmtPtr body;
};

struct Program {
    std::vector<Function> functions;
};

} // namespace scpp
