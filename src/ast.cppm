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

// A 1-based (line, column) position in the original source file, exactly
// like Clang/GCC's own diagnostics -- {0, 0} (the default) means "no
// location available" (a sentinel, since a real position is always >= 1;
// see lexer.cppm's line_/column_ initialization). Stamped onto Expr/Stmt/
// Function nodes by the parser (using the position of the first token
// each one syntactically begins with) and threaded through movecheck/
// codegen via DataflowState::current_loc / Codegen::current_loc_ so a
// thrown ParseError/DataflowError/CodegenError can always report exactly
// where its problem is -- see cli.cppm's print_diagnostic for how this
// gets rendered (a source-line excerpt with a caret, like `clang -c`).
struct SourceLocation {
    int line = 0;
    int column = 0;

    [[nodiscard]] bool is_known() const { return line > 0; }
};

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
    StringLiteral, // "hello\n" -- decoded byte content stored in `name`
                   // (same field Identifier/Call/Member reuse; see Expr
                   // below). Decays to a `char*` pointing at a compiler-
                   // emitted read-only global, exactly like a fixed-size
                   // char array decaying to pointer (ch03) -- there is no
                   // backing local variable/place, so (like every other
                   // literal) it has no codegen_lvalue case.
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

    // Where this expression begins in the source file (see
    // SourceLocation) -- stamped by the parser, used only for diagnostics
    // (movecheck/codegen error messages), never consulted by any actual
    // check.
    SourceLocation loc;

    // IntegerLiteral, or CharLiteral's ordinal value (e.g. 'a' -> 97) --
    // sharing this field rather than adding a new one keeps Expr flat;
    // which literal kind `expr.kind` is tells the two apart.
    long long int_value = 0;

    // BoolLiteral
    bool bool_value = false;

    // Identifier / Call (callee name) / Member (field name) / StringLiteral
    // (decoded byte content, e.g. "a\n" -> the 2 bytes 'a','\n' -- same
    // escape set as CharLiteral, see parser's decode_string_literal)
    std::string name;

    // Binary; also Call's method-call receiver (ch05 §5.9), nullptr for
    // an ordinary free-function call -- `obj.method(args)` parses to a
    // Call with `lhs = obj`, `name = "method"`, resolved to a concrete
    // synthesized function symbol only once `obj`'s static type is known
    // (movecheck/codegen, not the parser -- see codegen_call's
    // Member-base handling)
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

    // Where this statement begins in the source file -- same purpose as
    // Expr::loc above.
    SourceLocation loc;

    // VarDecl
    Type type;
    std::string var_name;
    ExprPtr init; // optional

    // VarDecl, class-typed only (ch04 §4.2): `ClassName name(args);`,
    // direct-initialization via an explicit constructor call --
    // mutually exclusive with `init` above (a class type has no
    // `=`-initializer form in this version, only this paren-args form or
    // a bare, zero-initialized declaration calling no constructor at
    // all). `has_ctor_args` is needed to tell an explicit-but-empty call
    // (`ClassName name();`) apart from no call at all (a bare
    // `ClassName name;`) -- `ctor_args` alone being empty can't
    // distinguish those two.
    bool has_ctor_args = false;
    std::vector<ExprPtr> ctor_args;

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
    // non-`safe` function), and tells codegen to skip its own runtime
    // checks (span bounds, integer overflow -- ch05 §5.8/ch08 Q1), for
    // the statements directly and transitively nested inside it; every
    // other check (ch05.1-5.4) keeps running unconditionally regardless
    // of this flag. Meaningless for every other StmtKind. Movecheck
    // separately rejects this flag being set at all when the enclosing
    // function isn't itself `safe` (a native function's entire body is
    // already an implicit unsafe context, so the marker has nothing left
    // to relax -- see check_function).
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
    // Where this function's declaration begins -- same purpose as
    // Expr::loc/Stmt::loc, for diagnostics that are about the function
    // itself (e.g. "function 'f' cannot return class 'X' by value")
    // rather than a specific statement/expression inside it.
    SourceLocation loc;
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

enum class AccessSpecifier {
    Public,
    Private,
};

struct ClassField {
    Type type;
    std::string name;
    // ch04 §4.2: a member variable can never be Public -- rejected right
    // where it's parsed (parse_class_def), not deferred to a later pass
    // -- but the specifier is still recorded here (rather than simply
    // never representing it) so this stays a plain, uniform data shape,
    // matching StructField's own style.
    AccessSpecifier access = AccessSpecifier::Private;
};

// ch04 §4.2 / ch05 §5.9: unlike `struct` (a purely trivial aggregate, ch04
// §4.1), `class` may own resources, participates in move/borrow checking,
// and restricts field access. A constructor/destructor/method's *body* is
// not represented here at all -- each is lowered directly into an
// ordinary top-level `Program::functions` entry at parse time (see
// parse_class_def), since ch05 §5.9 treats `this` as nothing more than an
// implicit Reference-typed first `Param` (`const T&` in a `const` method,
// `T&` otherwise) -- every existing reference/borrow-checking mechanism
// (elision, dangling checks, alias-XOR-mutability) already applies with
// zero new logic once a method is just a `Function` shaped this way.
// scpp has no real C++ name mangling, so these synthesized functions use
// a simple, deterministic `ClassName_memberName` scheme (`ClassName_new`
// for the constructor, `ClassName_delete` for the destructor) -- method
// calls (`obj.method(args)`) and constructor calls (`ClassName obj(args);`)
// both resolve to it by recomputing the identical scheme from the
// receiver's/declared variable's static type, not by consulting this
// struct or any separate registry.
struct ClassDef {
    std::string name;
    std::vector<ClassField> fields;
};

struct Program {
    std::vector<StructDef> structs;
    std::vector<ClassDef> classes;
    std::vector<Function> functions;
};

} // namespace scpp
