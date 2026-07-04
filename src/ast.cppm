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
    Reference, // T& (mutable borrow) / const T& (shared borrow) -- see ch05.2
    Span,      // std::span<T> (mutable view) / std::span<const T> (read-only
               // view) -- a non-owning, lifetime-checked {pointer, size}
               // view over a fixed-size array (see ch03/ch06; M6).
};

// A type reference. `pointee`/`element` use shared_ptr (not unique_ptr) so
// Type stays copyable: Param/StructField/Stmt store Type by value, and
// copying a pointer/array type is just a cheap refcount bump, not a deep
// clone.
struct Type {
    TypeKind kind = TypeKind::Named;

    // Named
    std::string name;

    // Pointer / UniquePtr / Reference / Span (element/referent type)
    std::shared_ptr<Type> pointee;

    // Array
    std::shared_ptr<Type> element;
    long long array_size = 0;

    // Reference: true for `T&` (mutable/exclusive borrow), false for
    // `const T&` (shared borrow). Span: true for `std::span<T>` (mutable
    // view), false for `std::span<const T>` (read-only view) -- reuses
    // this same flag rather than adding a new one, since the two types
    // share the same "is this view/borrow read-only" meaning. Meaningless
    // for every other kind.
    bool is_mutable_ref = true;

    // Pointer only: true for `T*`, false for `const T*` -- mirrors
    // is_mutable_ref above, but kept as its own separate flag (rather than
    // reusing is_mutable_ref) since Pointer's const-ness has different
    // rules from Reference's: `T*` converts implicitly to `const T*`
    // (widening) but never the reverse (no const_cast/.cast_mut()
    // equivalent in v0.1), whereas a reference is never converted at all
    // (see ch05 §5.7, ch08 Q9). Writing through a `const T*` (is_mutable_
    // pointee == false) is rejected unconditionally, even inside
    // `unsafe { }` -- see movecheck's assignment_target_is_read_only.
    bool is_mutable_pointee = true;
};

enum class ExprKind {
    IntegerLiteral,
    BoolLiteral,
    CharLiteral, // 'a', '\n', ... -- ordinal value stored in `int_value`
                 // (same field as IntegerLiteral; see Expr below)
    Identifier,
    Binary,
    Unary,
    Call,
    Member,
    Subscript,
    Move,       // std::move(x) -- compiler builtin move hint, not an ordinary call
    MakeUnique, // std::make_unique<T>(args...) -- compiler builtin heap allocation
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
    Deref, // `*p` -- p must be std::unique_ptr<T> (always allowed) or a raw
           // pointer `T*` (only inside `unsafe { }`, see ch01 §1.3/movecheck's
           // validate_deref_operand). A reference dereference makes no
           // sense here and never reaches this: a reference already *is*
           // its referent (see codegen_lvalue's auto-deref).
    AddressOf, // `&expr` -- always legal in a `safe` function (no `unsafe {}`
               // needed to *create* a raw pointer, only to dereference one --
               // ch05 §5.7). `expr` must be one of the same forms accepted as
               // a borrow source for `T&`/`const T&` (ch05.2): a plain local/
               // parameter, a `.field`/`[index]` projection, or `*p`/`p->x`
               // off a std::unique_ptr. Evaluates to a `T*`, registering no
               // lasting borrow (see movecheck's apply_address_of).
};

// A single expression node. Only the fields relevant to `kind` are populated;
// this keeps the AST as a flat, easy-to-pattern-match tagged union without
// needing a class hierarchy for the minimal M1 subset.
struct Expr {
    ExprKind kind;

    // IntegerLiteral, or CharLiteral's ordinal value (e.g. 'a' -> 97) --
    // sharing this field rather than adding a new one keeps Expr flat;
    // which literal kind `expr.kind` is tells the two apart.
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

    // Call arguments / MakeUnique constructor arguments
    std::vector<ExprPtr> args;

    // MakeUnique: the allocated element type `T` in `make_unique<T>(...)`.
    Type type;

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
    // Block: true for an `unsafe { }` block (ch01 §1.3), false for an
    // ordinary `{ }`. An unsafe block is otherwise a completely normal
    // Block -- same lexical scoping, same statement list -- this flag
    // only tells the move checker to relax the specific ch05.5 checks
    // it's licensed to relax (raw pointer dereference, calling a
    // non-`safe` function) for the statements directly and transitively
    // nested inside it; every other check (ch05.1-5.4) keeps running
    // unconditionally regardless of this flag. Meaningless for every
    // other StmtKind.
    bool is_unsafe = false;
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
    // Null for a bodyless `extern "C"` declaration (ch02 §2.1) -- defined
    // elsewhere, linked in externally. Always non-null for every other
    // function (an ordinary definition, or an `extern "C"` *definition*
    // with a body). Nothing outside parsing/movecheck/codegen's
    // extern-declaration handling should assume this is always non-null.
    StmtPtr body;
    // ch02 §2.1: requests C linkage. Orthogonal to `is_safe` when `body`
    // is present (a `safe extern "C"` definition is allowed and checked
    // like any other `safe` function); when `body` is null, `is_safe` is
    // always false (parsing rejects `safe` on a bodyless declaration,
    // since the compiler can't verify an implementation it can't see).
    bool is_extern_c = false;
    // ch02 §2.1: the declaration ends in a trailing `...` (e.g.
    // `printf(const char* fmt, ...)`). Parsed and stored, but v0.1
    // doesn't yet support a *call site* passing extra arguments beyond
    // `params` to such a function (see codegen's declare_function) --
    // only parsing/declaring the correct variadic signature shape is
    // implemented in this first slice, per the spec's own scoping.
    bool has_varargs = false;
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
