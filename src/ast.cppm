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

enum class TypeKind {
    Named,     // scalar (int/bool) or a user-declared struct name
    Pointer,   // T*
    Array,     // T[N]
    UniquePtr, // std::unique_ptr<T> -- unique ownership, move-only (see ch05)
};

// A type reference. `pointee`/`element` use shared_ptr (not unique_ptr) so
// Type stays copyable: Param/StructField/Stmt store Type by value, and
// copying a pointer/array type is just a cheap refcount bump, not a deep
// clone.
struct Type {
    TypeKind kind = TypeKind::Named;

    // Named
    std::string name;

    // Pointer / UniquePtr (element type)
    std::shared_ptr<Type> pointee;

    // Array
    std::shared_ptr<Type> element;
    long long array_size = 0;
};

enum class ExprKind {
    IntegerLiteral,
    BoolLiteral,
    Identifier,
    Binary,
    Unary,
    Call,
    Member,
    Subscript,
    Move, // std::move(x) -- compiler builtin move hint, not an ordinary call
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

    // Identifier / Call (callee name) / Member (field name)
    std::string name;

    // Binary
    BinaryOp binary_op{};
    ExprPtr lhs;
    ExprPtr rhs;

    // Unary (operand stored in `lhs`)
    UnaryOp unary_op{};

    // Call arguments
    std::vector<ExprPtr> args;

    // Member: object stored in `lhs`, field name in `name`.
    // Subscript: array/collection stored in `lhs`, index expr in `rhs`.
    // Move: moved expression stored in `lhs` (must resolve to a plain
    // local variable of unique_ptr type; enforced by the move checker,
    // not the parser).
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
    Type type;
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
    Type type;
    std::string name;
};

struct Function {
    bool is_safe = false;
    Type return_type;
    std::string name;
    std::vector<Param> params;
    StmtPtr body;
};

struct StructField {
    Type type;
    std::string name;
};

struct StructDef {
    std::string name;
    std::vector<StructField> fields;
};

struct Program {
    std::vector<StructDef> structs;
    std::vector<Function> functions;
};

} // namespace scpp
