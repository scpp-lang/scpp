module;

export module scpp.ast;

import std;

export namespace scpp {

class Expr;
class Stmt;
class SwitchCase;
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
class SourceLocation {
  public:
    virtual ~SourceLocation() = default;
    SourceLocation() = default;
    SourceLocation(int line, int column, std::shared_ptr<const std::string> source_path = {})
        : line{line}, column{column}, source_path{std::move(source_path)} {}
    SourceLocation(const SourceLocation&) = default;
    SourceLocation& operator=(const SourceLocation&) = default;

    int line = 0;
    int column = 0;
    std::shared_ptr<const std::string> source_path;

    [[nodiscard]] bool is_known() const { return line > 0; }
    [[nodiscard]] bool has_source_path() const {
        if (source_path == nullptr) return false;
        return source_path.operator*().size() != 0;
    }

    [[nodiscard]] const std::string& source_path_text() const {
        static const std::string empty;
        if (source_path == nullptr) return empty;
        return source_path.operator*();
    }

};

[[nodiscard]] inline SourceLocation make_source_location(int line, int column,
                                                        std::shared_ptr<const std::string> source_path = {}) {
    SourceLocation loc{};
    loc.line = line;
    loc.column = column;
    loc.source_path = std::move(source_path);
    return loc;
}

enum class ReceiverRefQualifier {
    None,
    LValue,
    RValue,
};

enum class TypeKind {
    Named,     // scalar (int/bool) or a user-declared struct name
    Pointer,   // T*
    Function,  // Ret(Args...) as a symbolic type argument / specialization pattern
    FunctionPointer, // Ret (*p)(Args...) / Ret (* [[scpp::unsafe]] p)(Args...)
    Array,     // T[N]
    Reference, // T& (mutable borrow) / const T& (shared borrow) -- see ch05.2
    Span,      // std::span<T> (mutable view) / std::span<const T> (read-only
               // view) -- a non-owning, lifetime-checked {pointer, size}
               // view over a fixed-size array (see ch03/ch06; M6).
};

class LifetimeAnnotation {
  public:
    virtual ~LifetimeAnnotation() = default;
    LifetimeAnnotation() = default;
    LifetimeAnnotation(const LifetimeAnnotation&) = default;
    LifetimeAnnotation& operator=(const LifetimeAnnotation&) = default;
    LifetimeAnnotation(LifetimeAnnotation&&) = default;
    LifetimeAnnotation& operator=(LifetimeAnnotation&&) = default;

    // Empty when no `[[scpp::lifetime(...)]]` is present on this
    // declaration. Otherwise the raw identifier spelled in source --
    // either the reserved word `any` or a user-written, declaration-
    // local group name.
    std::string name;

    [[nodiscard]] bool present() const { return name.size() != 0; }
    [[nodiscard]] bool is_any() const { return name == "any"; }

};

// A type reference. `pointee`/`element` use shared_ptr (not unique_ptr) so
// Type stays copyable: Param/StructField/Stmt store Type by value, and
// copying a pointer/array type is just a cheap refcount bump, not a deep
// clone.
class Type {
  public:
    virtual ~Type() = default;
    Type() = default;
    Type(const Type& other);
    Type& operator=(const Type& other);
    Type(Type&&) = default;
    Type& operator=(Type&&) = default;

    TypeKind kind = TypeKind::Named;
    LifetimeAnnotation lifetime;

    // Named
    std::string name;

    // Pointer / Reference / Span (element/referent type)
    std::shared_ptr<Type> pointee;

    // Array
    std::shared_ptr<Type> element;
    // ch05 §9.4: the array's bound, once resolved -- meaningless (0) until
    // then. A freshly-parsed array declarator never sets this directly
    // any more; it always starts out only as `array_size_expr` below,
    // which the constexpr engine's array-bound resolution pass
    // (constexpression.cppm's AlignmentResolver, extended for ch05 §9.4)
    // evaluates and validates (a converted constant expression of
    // `std::size_t`, strictly > 0) before clearing `array_size_expr`
    // back to null and storing the resulting value here -- mirroring how
    // `template_args` above is resolved in place and then cleared. Every
    // consumer downstream of that pass (layout_of_type, codegen, ...)
    // may assume `array_size_expr == nullptr` and read this field
    // directly.
    std::int64_t array_size = 0;
    // ch05 §9.4: the not-yet-evaluated array-bound constant-expression
    // exactly as parsed (e.g. `sizeof(T)`, `alignof(Header) * 2`, a bare
    // `constexpr` constant's name, or simply an integer literal) --
    // non-null only between parsing and array-bound resolution. A
    // shared_ptr (not unique_ptr) for the same reason as `non_type_args`
    // below: Type must stay copyable (Param/StructField/Stmt store Type
    // by value), so an ordinary Type copy just shares this expression
    // harmlessly -- nothing may ever mutate `*array_size_expr` in place.
    // Monomorphization's generic-parameter substitution (e.g. resolving
    // `sizeof(T)` for `Box<int>`) instead clones this expression,
    // substitutes the concrete type into the clone, and assigns a
    // *new* shared_ptr here, exactly like `substitute_type_param`
    // already does for `pointee`/`element` -- never modifies the
    // original template's own expression object.
    std::shared_ptr<Expr> array_size_expr;

    // Function / FunctionPointer
    std::shared_ptr<Type> function_return;
    std::vector<Type> function_params;
    bool is_unsafe_function_pointer = false;
    // Function only: cv/ref-qualifiers on a symbolic function type
    // template argument / partial-specialization pattern, e.g.
    // `void() const`, `void() &`, `void() &&`.
    bool is_const_function = false;
    ReceiverRefQualifier function_ref_qualifier = ReceiverRefQualifier::None;

    // Reference: true for `T&` (mutable/exclusive borrow), false for
    // `const T&` (shared borrow). Span: true for `std::span<T>` (mutable
    // view), false for `std::span<const T>` (read-only view) -- reuses
    // this same flag rather than adding a new one, since the two types
    // share the same "is this view/borrow read-only" meaning. Meaningless
    // for every other kind. Not consulted when is_rvalue_ref (below) is
    // true.
    bool is_mutable_ref = true;

    // Reference only: true for an rvalue-reference spelling (`T&&` or a
    // generic `auto&&`/deduced-`T&&` parameter before deduction). A
    // non-deduced `T&&` remains the ordinary "passed by move" form; a
    // deduced forwarding reference may later collapse to `T&` when the
    // argument is an lvalue, but the parsed spelling still records `&&`
    // here. false for `T&`/`const T&`, where is_mutable_ref (above) then
    // distinguishes those two as before.
    bool is_rvalue_ref = false;

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

    // Top-level `const` on a non-reference, non-pointer type, e.g.
    // `const std::string` when used as a generic/template type argument.
    // Distinct from Stmt::is_const, which marks an immutable local
    // variable declaration rather than a type-level qualifier.
    bool is_const_qualified = false;
    // True only when this spelled type is one of the special by-value
    // wrappers whose carried referent may serve as a lifetime source for
    // pointer-return inference/validation: `std::reference_wrapper<T>`
    // itself, or an allowed wrapper that contains exactly one such value
    // (today `std::optional<std::reference_wrapper<T>>`). This does not
    // make the wrapper object borrow-tracked in general; it is consulted
    // only by the cross-function lifetime machinery.
    bool is_reference_wrapper_lifetime_source = false;

    // ch05 §5.14: non-empty only for a *not-yet-resolved* generic-type
    // instantiation, e.g. `Vec<int>` parsed as `Type{Named, "Vec"}` with
    // `template_args == [Type{Named,"int"}]` -- `name` still names the
    // *template*, not a real, concrete type. Resolved in place by
    // movecheck's Monomorphizer pass (the same pre-check_moves phase that
    // resolves a Lambda literal's own synthesized class and a generic
    // function's call-site clone): looks up the original template
    // ClassDef/StructDef, synthesizes a concrete instantiation, and
    // rewrites `name` to the mangled concrete name while clearing this
    // back to empty -- mirroring the "auto" VarDecl-type sentinel and a
    // Lambda's own `name`-starts-empty-until-resolved pattern. Never
    // reaches check_moves/codegen non-empty. An ordinary (non-variadic)
    // generic type always populates this with exactly one entry (its own
    // single type parameter); a variadic one (Tuple/TupleImpl-style, ch05
    // §5.14) populates it with the *type*-parameter-position arguments
    // only (its own pack elements) -- see non_type_args below for the
    // separate, non-type-parameter-position arguments (e.g. TupleImpl's
    // own leading "Idx").
    std::vector<Type> template_args;
    // ch05 §5.14: a variadic generic type's own NON-TYPE argument(s)
    // (e.g. the "0" in `TupleImpl<0, int, bool, char>`, or the "Idx + 1"
    // expression in a specialization's own base-clause spread
    // `TupleImpl<Idx + 1, Tail...>`) -- always logically *before*
    // template_args, matching the established shape every variadic
    // generic type's own header uses (0+ leading non-type parameters,
    // then a type parameter optionally followed by a pack; see
    // GenericTypeParam's own comment). A shared_ptr (not unique_ptr) so
    // Type itself stays copyable, matching pointee/element's own
    // existing choice -- needed since Type values are copied freely
    // throughout movecheck.cppm (see its own reference-invalidation-
    // safety comments). Each entry is restricted to a small, purpose-
    // scoped expression shape (an integer literal, a bare identifier
    // naming an enclosing template's own non-type parameter, or a `+`
    // of the two) -- not a general compile-time constant-expression
    // evaluator; see movecheck's evaluate_non_type_arg.
    std::vector<std::shared_ptr<Expr>> non_type_args;
    // ch05 §5.14: true only for one, special `template_args` entry --
    // `Name{Named, is_pack_expansion=true}` -- meaning "spread the
    // enclosing generic *function* template's own pack parameter named
    // `Name` here" (e.g. the trailing "Tail..." in a base-class-
    // deduction accessor's own parameter type,
    // `TupleImpl<I, Head, Tail...>& t`, ch05 §5.14's `get<I>` pattern).
    // Always the *last* entry, mirroring GenericTypeParam::is_pack's own
    // "pack is always last" rule. Meaningless anywhere `template_args`
    // holds already-concrete arguments (an ordinary use-site
    // instantiation like `Tuple<int, bool, char>` never sets this --
    // there, every pack element is spelled out individually as an
    // ordinary concrete type). Left entirely unresolved until
    // movecheck's base-class-deduction algorithm substitutes the
    // enclosing function template's own concrete Tail binding in place.
    bool is_pack_expansion = false;
};

class AlignmentSpecifier {
  public:
    virtual ~AlignmentSpecifier() = default;
    SourceLocation loc;
    bool operand_is_type = false;
    Type type;
    ExprPtr expr;

    AlignmentSpecifier() = default;
    AlignmentSpecifier(const AlignmentSpecifier& other);
    AlignmentSpecifier& operator=(const AlignmentSpecifier& other);
    AlignmentSpecifier(AlignmentSpecifier&&) = default;
    AlignmentSpecifier& operator=(AlignmentSpecifier&&) = default;
};

[[nodiscard]] inline Type named_type(std::string name) {
    Type type{};
    type.kind = TypeKind::Named;
    type.name = std::move(name);
    return type;
}

// ch06 §6: the canonical internal spelling of `nullptr`'s type. Source
// may write it either bare (`nullptr_t`) or `std::`-qualified
// (`std::nullptr_t`, the spelling real C++ exposes) -- the parser
// normalizes both to this single name, so every later phase compares
// against one string rather than two. Mirrors the way `size_t` is
// stored unqualified even when written `std::size_t`.
[[nodiscard]] inline std::string nullptr_type_name() { return "nullptr_t"; }

// `nullptr_t` as a ready-made Type -- the form most callers actually
// want (type inference returns one, the parser stores one).
[[nodiscard]] inline Type nullptr_named_type() { return named_type(nullptr_type_name()); }

// Whether `type` is `nullptr`'s own type -- the type of the null
// pointer literal, whose only value is that literal. It converts to any
// raw pointer type and to any class declaring a converting constructor
// that takes it; it converts to nothing else (in particular to no
// integer type and to no `bool` -- see ch06 §6).
[[nodiscard]] inline bool is_nullptr_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name == nullptr_type_name();
}

class Param {
  public:
    virtual ~Param() = default;
    Param() = default;
    Param(const Param& other);
    Param& operator=(const Param& other);
    Param(Param&&) = default;
    Param& operator=(Param&&) = default;

    Type type;
    std::string name;
    // Name resolution's result for a parameter: the identity of the local
    // this parameter introduces, encoded exactly like
    // Expr::resolved_local (`id + 1`, zero meaning unresolved). A
    // parameter is a declaration like any other, so it carries its own
    // id here rather than being reachable only through the declaration
    // table -- that is what lets a consumer with no table in hand (most
    // of codegen: `this`, a defaulted operator's `other`, the
    // parameter-teardown pass) name the right storage without falling
    // back to a lookup by spelling.
    std::size_t resolved_local = 0;
    LifetimeAnnotation lifetime;
    std::shared_ptr<Expr> default_expr;
    // ch05 §5.11: empty for an ordinary parameter (the overwhelmingly
    // common case). Non-empty names the concept this parameter is
    // constrained by, for the abbreviated generic-function form --
    // `ConceptName auto name` (by value), `ConceptName auto& name`/
    // `const ConceptName auto& name` (mutable/shared borrow), or
    // `ConceptName auto&& name` (a forwarding-reference spelling that
    // collapses at call resolution) -- mirroring the ordinary `T`/`T&`/
    // `const T&`/`T&&` forms exactly, just with `auto` interposed and a
    // concept name standing in for a concrete type (see
    // parse_generic_param_type). `type` itself is still fully
    // populated the same shape an ordinary parameter's would be, except
    // its innermost Named type names a synthesized *witness class*
    // (see ClassDef::is_concept_witness) rather than a real type -- the
    // generic function's own body is checked once, abstractly, against
    // that witness (ch05 §5.11); this field alone records *which*
    // concept produced it, consulted at each call site to monomorphize
    // (see the parser's/movecheck's concept-monomorphization pass).
    std::string generic_concept;
    // ch05 §5.15: `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`
    // attached to a (generic) parameter's own declaration -- constrains
    // its (possibly template-deduced) type to satisfy the corresponding
    // structural property, checked at each call site against the
    // deduced/concrete argument type (mirrors Rust's `Send`/`Sync`
    // trait bounds on a generic function's own type parameter). Only
    // meaningful on a parameter whose type actually depends on one of
    // the enclosing function's own template parameters -- see
    // Monomorphizer's own check_thread_safety_constraint.
    bool require_thread_movable = false;
    bool require_thread_shareable = false;
    // ch05 §5.11: true only for the trailing abbreviated generic pack form
    // (`Concept auto&... args`). Supported only on a free function's own
    // parameter list, never on a method/lambda in this version.
    bool is_parameter_pack = false;
};

// ch05 §5.12: one entry in a lambda expression's own capture-list --
// `[x]` (by-value), `[&x]` (by-reference), `[x = expr]`/`[&x = expr]`
// (init-capture: the field's initial value/binding is `expr`, evaluated
// in the *enclosing* scope, rather than a copy/reference of an existing
// same-named local -- how a move-only type crosses into a closure, e.g.
// `[p = std::move(p)]`), or `[this]` (name == "this", captures a
// reference to the enclosing method's own receiver). Populated for
// every *explicit* capture at parse time (parser.cppm's
// parse_lambda_expression); a blanket `[=]`/`[&]` capture mode (see
// Expr::lambda_blanket_mode) adds further *implicit* entries to this
// same list, but only once movecheck's closure-resolution pass (which
// alone has the per-function type information needed to know what a
// blanket capture's free variables even refer to) has run -- by the
// time movecheck's own per-function checking or codegen ever sees a
// Lambda Expr node, this list is always the complete, final capture set.
class LambdaCapture {
  public:
    virtual ~LambdaCapture() = default;
    LambdaCapture() = default;
    LambdaCapture(const LambdaCapture& other);
    LambdaCapture& operator=(const LambdaCapture& other);
    LambdaCapture(LambdaCapture&&) = default;
    LambdaCapture& operator=(LambdaCapture&&) = default;

    std::string name;
    bool by_reference = false;
    // Non-null only for an init-capture; null for a plain `[name]`/
    // `[&name]` capture (whose value/binding comes directly from the
    // enclosing scope's own same-named local).
    ExprPtr init;
    // Name resolution's result for the *enclosing* function's local this
    // capture names -- see Expr::resolved_local for the encoding.
    std::size_t resolved_local = 0;
};

enum class LambdaCaptureMode {
    None,       // only the explicitly-listed captures -- e.g. `[]`, `[x]`.
    ByValue,    // `[=]` or a mixed `[=, &y]` -- every other free variable
                // referenced in the body is implicitly captured by value.
    ByReference, // `[&]` or a mixed `[&, x]` -- every other free variable
                 // referenced in the body is implicitly captured by
                 // reference.
};

enum class ExprKind {
    IntegerLiteral,
    // A floating-point literal (`1.5`) -- value stored in `float_value`
    // (ch06 §6: defaults to a bare `double`-typed prvalue, same as real
    // C++ with no suffix, adapted to a narrower/other float type by
    // context wherever one is known, e.g. a VarDecl's own declared type
    // -- see codegen's codegen_value_for_target).
    FloatLiteral,
    BoolLiteral,
    // `nullptr` (ch06 §6) -- the null pointer literal. Its type is the
    // builtin `nullptr_t` (spelled `std::nullptr_t` too), which converts
    // to any raw pointer type and to any class type that declares a
    // converting constructor taking it, and to nothing else. Carries no
    // payload: `nullptr_t` has exactly one value, so there is no
    // int_value/bool_value analogue to store.
    //
    // A dedicated kind rather than an Identifier named "nullptr" (which
    // is what this used to be): as a bare Identifier the literal had no
    // type at all, so every consumer either special-cased the spelling
    // or reported it as an undeclared variable.
    NullptrLiteral,
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
    Conditional, // `cond ? then_expr : else_expr` -- `lhs` is the condition,
                 // `rhs` the then-arm, `third` the else-arm.
    Unary,
    Call,
    Member,
    Subscript,
    New,        // `new T` / `new T(args...)` -- raw heap allocation, gated by
                // `[[scpp::unsafe]]`, returning `T*` (spec §5.1(5.4)).
    Delete,     // `delete expr` -- destroys the pointed-to object (if any) and
                // frees its storage; also gated by `[[scpp::unsafe]]`.
    Destroy,    // `expr.~T()` / `ptr->~T()` -- explicit destructor call without
                // deallocation, gated by `[[scpp::unsafe]]`.
    Move,       // std::move(x) -- compiler builtin move hint, not an ordinary call
    TypeTrait,  // scpp::is_thread_movable(T) / scpp::is_thread_shareable(T) --
                // compiler builtin type-trait predicates whose queried type
                // lives in `type` and whose specific trait name lives in
                // `name`; evaluates to a bool constant.
    PackExpansion, // `expr...` in a generic function body, currently only
                   // meaningful inside a call/new/constructor argument list
                   // before monomorphization expands it to concrete args.
    Lambda,     // `[captures](params) { body }` (ch05 §5.12) -- desugars to
                // constructing an anonymous, compiler-synthesized class; see
                // Expr's own lambda_* fields below.
    Fold,       // C++17 fold expression over a parameter pack (ch05 §5.11),
                // e.g. `(args + ...)`, `(... + args)`, or `(args + ... + 0)`.
    Cast,       // `static_cast<T>(expr)` / `(T)expr` (ch06 §6) -- an explicit
                // scalar-to-scalar conversion; the *only* way to convert
                // between two distinct scpp scalar types (no implicit
                // conversion exists anywhere else). Target type `T` stored in
                // `type`, operand in `lhs` -- see Expr's own comment.
    Alignof,    // `alignof(T)` -- target-ABI alignment in bytes of a type-id,
                // stored in `type`.
    Sizeof,     // `sizeof(T)` / `sizeof(expr)` -- target-ABI size in bytes of
                // either a type operand (stored in `type`) or an unevaluated
                // expression operand (stored in `lhs`), distinguished by
                // `sizeof_operand_is_type` below.
    ValueInit,  // A bare `{}` used as a complete expression (e.g.
                // `return {};`) rather than as a postfix constructor
                // applied to a named type (`TypeName{}`, which parses as
                // an ordinary Call instead -- see parse_return's existing
                // Identifier+LBrace special case). Only recognized where
                // the target type is otherwise unambiguous from context
                // (currently: a return statement, whose enclosing
                // function's own declared return type is stamped onto
                // `type` by the parser); value-initializes that type,
                // identically to spelling it out explicitly as
                // `ReturnType{}` would (zero for scalars, each member's
                // own default per its in-class initializer/default
                // constructor for a class/struct -- ch04/ch05 §5.1).
};

enum class BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    AddAssign,
    SubAssign,
    MulAssign,
    DivAssign,
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
    PreInc,
    PreDec,
    PostInc,
    PostDec,
    Deref, // `*p` -- either a raw pointer `T*` (only inside `unsafe { }`,
           // see ch01 §1.3/movecheck's validate_deref_operand) or a user
           // class type whose `operator*` method movecheck desugars this
           // to call. A reference dereference makes no sense here and
           // never reaches this: a reference already *is* its referent
           // (see codegen_lvalue's auto-deref).
    AddressOf, // `&expr` -- always legal (no `unsafe {}` needed to *create*
               // a raw pointer, only to dereference one -- ch05 §5.7).
               // `expr` must be one of the same forms accepted as
               // a borrow source for `T&`/`const T&` (ch05.2): a plain local/
               // parameter, a `.field`/`[index]` projection, or a
               // dereference/member access that ultimately resolves back to
               // one of those roots. Evaluates to a `T*`, registering no
               // lasting borrow (see movecheck's apply_address_of).
};

// ch05 §5.11/§5.14: one explicit argument in a generic function call's
// own `<...>` list (e.g. `make<Circle>()`'s "Circle", or `get<2>(t)`'s
// "2") -- needed for a full-header-form generic function (Function::
// template_params) whose template parameter either has no corresponding
// function-parameter position at all (a "return-type-only" generic) or
// must be supplied explicitly to drive base-class deduction (ch05
// §5.14's `get<I>` accessor pattern) rather than deduced from an
// ordinary argument's own type. Exactly one of `type`/`value` is
// meaningful, per `is_type`; `value` is restricted to the same small,
// purpose-scoped expression shape as Type::non_type_args (an integer
// literal, or a `+` of one) -- see movecheck's evaluate_non_type_arg.
class ExplicitTemplateArg {
  public:
    virtual ~ExplicitTemplateArg() = default;
    ExplicitTemplateArg() = default;
    ExplicitTemplateArg(const ExplicitTemplateArg& other);
    ExplicitTemplateArg& operator=(const ExplicitTemplateArg& other);
    ExplicitTemplateArg(ExplicitTemplateArg&&) = default;
    ExplicitTemplateArg& operator=(ExplicitTemplateArg&&) = default;

    bool is_type = true;
    Type type;                 // meaningful when is_type
    std::shared_ptr<Expr> value; // meaningful when !is_type
};

// A single expression node. Only the fields relevant to `kind` are populated;
// this keeps the AST as a flat, easy-to-pattern-match tagged union without
// needing a class hierarchy for the minimal M1 subset.
class Expr {
  public:
    virtual ~Expr() = default;
    Expr() = default;
    Expr(const Expr& other);
    Expr& operator=(const Expr& other);
    Expr(Expr&&) = default;
    Expr& operator=(Expr&&) = default;

    ExprKind kind{};

    // Name resolution's result for an `Identifier` naming a local
    // variable (or parameter) of the enclosing function: `id + 1`, where
    // `id` indexes that function's Body::local_decls (see mir.cppm).
    // Zero means "not resolved to a local" -- either this isn't an
    // Identifier at all, or the name refers to something that isn't a
    // local (a global, a function, an enum constant), or resolution
    // hasn't run yet. The `+ 1` bias exists so the default-constructed
    // value is the "none" case without needing a cast to form a
    // sentinel; use mir.cppm's has_resolved_local/resolved_local_of
    // rather than doing the arithmetic by hand.
    //
    // This lives on the AST node (rather than in a side table keyed by
    // node address) because a single source-level function body exists
    // as several physically distinct trees at once: build_mir lowers a
    // deep clone, while monomorphize walks a *second* clone and
    // dataflow/interfaces walk the original. A resolution carried by the
    // node itself survives deep_clone_expr and so stays correct in every
    // copy; an address-keyed map would resolve in only one of them.
    std::size_t resolved_local = 0;

    // Where this expression begins in the source file (see
    // SourceLocation) -- stamped by the parser, used only for diagnostics
    // (movecheck/codegen error messages), never consulted by any actual
    // check.
    SourceLocation loc{};

    // IntegerLiteral, or CharLiteral's ordinal value (e.g. 'a' -> 97) --
    // sharing this field rather than adding a new one keeps Expr flat;
    // which literal kind `expr.kind` is tells the two apart.
    std::int64_t int_value = 0;

    // FloatLiteral
    double float_value = 0.0;

    // BoolLiteral
    bool bool_value = false;

    // Identifier / Call (callee name) / Member (field name) / StringLiteral
    // (decoded byte content, e.g. "a\n" -> the 2 bytes 'a','\n' -- same
    // escape set as CharLiteral, see parser's decode_string_literal) /
    // Lambda (the synthesized concrete class this literal constructs --
    // empty until movecheck's closure-resolution pass runs, see
    // lambda_captures's own comment; non-empty by the time codegen ever
    // sees this node).
    std::string name{};

    // Identifier / Call only: true when the source spelled this name
    // with a leading `::`, forcing lookup from the global namespace
    // rather than the current enclosing namespace.
    bool explicit_global_qualification = false;

    // Binary; also Call's method-call receiver (ch05 §5.9), nullptr for
    // an ordinary free-function call -- `obj.method(args)` parses to a
    // Call with `lhs = obj`, `name = "method"`, resolved to a concrete
    // synthesized function symbol only once `obj`'s static type is known
    // (movecheck/codegen, not the parser -- see codegen_call's
    // Member-base handling). Fold uses `binary_op` as the folded operator;
    // `lhs` is the sole operand for unary folds and the left-side operand
    // for binary folds, `rhs` the optional right-side operand for a binary
    // fold, and `fold_ellipsis_on_left` distinguishes `(... op pack)` from
    // `(pack op ...)`.
    BinaryOp binary_op{};
    ExprPtr lhs{};
    ExprPtr rhs{};
    ExprPtr third{};
    bool fold_ellipsis_on_left = false;

    // Unary (operand stored in `lhs`)
    UnaryOp unary_op{};

    // Call arguments / New constructor arguments
    std::vector<ExprPtr> args{};

    // ch05 §5.11/§5.14: Call only -- non-empty only for a call to a
    // full-header-form generic function that explicitly supplies one or
    // more of its own template arguments at the call site (e.g.
    // `make<Circle>()`, `get<2>(t)`) -- see ExplicitTemplateArg's own
    // comment. Empty for every other call (the overwhelmingly common
    // case: an ordinary, non-generic call, or a generic call resolved
    // entirely by argument-position deduction).
    std::vector<ExplicitTemplateArg> explicit_template_args{};

    // New: the allocated element type `T` in `new T...`.
    // Destroy: the explicitly-named destroyed type `T` in `expr.~T()` /
    // `ptr->~T()`.
    // Lambda: the explicit trailing return type (`-> Type`), only
    // meaningful when has_lambda_explicit_return_type is true.
    // Cast: the target type `T` in `static_cast<T>(expr)`/`(T)expr`
    // (operand stored in `lhs`, like Unary).
    // Sizeof(type): the queried type operand `T`.
    // ValueInit: the type to value-initialize (see ExprKind::ValueInit).
    Type type{};
    // Sizeof only: true when this node is the `sizeof(T)` form (queried type
    // stored in `type`), false for `sizeof(expr)` (unevaluated operand stored
    // in `lhs`).
    bool sizeof_operand_is_type = false;
    // New only: distinguishes `new T` (false) from `new T(...)` (true,
    // including the explicit-empty `new T()` form) so codegen can mirror
    // local-construction syntax's own "bare declaration vs explicit ctor
    // call" distinction.
    bool has_paren_init = false;
    // Destroy only: true for `ptr->~T()`, false for `obj.~T()`.
    bool destroy_through_pointer = false;
    // Member/Call only: true when the source spelled this access/call
    // with `->` rather than `.`, so the operator-> protocol still needs
    // to be resolved later once the receiver type is known.
    bool through_arrow = false;
    // Unary/Deref only: true for the compiler-synthesized final `*ptr`
    // used internally to complete one `E1->E2` expression after following
    // an operator-> chain. This pointer operand is never user-visible.
    bool implicit_arrow_deref = false;
    // Unary/Deref only, meaningful only when implicit_arrow_deref is
    // true: whether every selected operator-> step in that same chain was
    // receiver-tied, so the final implicit raw-pointer dereference is the
    // one safe carve-out that does not itself require an unsafe context.
    bool implicit_arrow_chain_safe = false;

    // Member: object stored in `lhs`, field name in `name`.
    // Subscript: array/collection stored in `lhs`, index expr in `rhs`.
    // Move: moved expression stored in `lhs` (must resolve to a plain
    // local variable of unique_ptr type; enforced by the move checker,
    // not the parser).

    // Lambda (ch05 §5.12) -- see LambdaCapture/LambdaCaptureMode's own
    // comments. `lambda_captures` holds every *explicit* capture at
    // parse time; movecheck's closure-resolution pass appends any
    // further *implicit* ones resolved from `lambda_blanket_mode`
    // in place, and sets `name` (above) to the synthesized concrete
    // class before movecheck's own per-function checking or codegen
    // ever runs.
    std::vector<LambdaCapture> lambda_captures{};
    LambdaCaptureMode lambda_blanket_mode = LambdaCaptureMode::None;
    // The lambda's own parameter list -- ordinary Params; a concept-
    // constrained lambda parameter is not supported in this version
    // (mirrors parser.cppm's reject_generic_params for methods).
    std::vector<Param> lambda_params{};
    bool has_lambda_explicit_return_type = false;
    // `[x](int y) mutable { ... }` -- licenses the synthesized "call"
    // method to modify by-value-captured fields (a non-`const` method,
    // mirroring an ordinary method's own trailing `const`, ch05 §5.9).
    bool lambda_is_mutable = false;
    StmtPtr lambda_body{};
};

enum class StmtKind {
    VarDecl,
    Return,
    If,
    While,
    Switch,
    Break,
    Continue,
    Fallthrough,
    ExprStmt,
    Block,
};

enum class FunctionEvalMode {
    RuntimeOnly,
    Constexpr,
    Consteval,
};

enum class IfMode {
    Runtime,
    ConstevalTrue,
    ConstevalFalse,
};

class SwitchCase {
  public:
    virtual ~SwitchCase() = default;
    SwitchCase() = default;
    SwitchCase(const SwitchCase& other);
    SwitchCase& operator=(const SwitchCase& other);
    SwitchCase(SwitchCase&&) = default;
    SwitchCase& operator=(SwitchCase&&) = default;

    SourceLocation loc{};
    ExprPtr value{}; // null => default
    std::vector<StmtPtr> statements{};
};

class Stmt {
  public:
    virtual ~Stmt() = default;
    Stmt() = default;
    Stmt(const Stmt& other);
    Stmt& operator=(const Stmt& other);
    Stmt(Stmt&&) = default;
    Stmt& operator=(Stmt&&) = default;

    StmtKind kind{};

    // Where this statement begins in the source file -- same purpose as
    // Expr::loc above.
    SourceLocation loc{};

    // VarDecl
    Type type{};
    std::string var_name{};
    // Name resolution's result for a VarDecl: the identity of the local
    // this declaration introduces, encoded exactly like
    // Expr::resolved_local (`id + 1`, zero meaning unresolved). Two
    // VarDecls that share a `var_name` in different scopes get *different*
    // ids -- that distinction is the whole point, and is what lets a later
    // pass refine one declaration's type (monomorphize's `auto`
    // resolution) without disturbing its namesakes.
    std::size_t declared_local = 0;
    ExprPtr init{}; // optional
    std::vector<AlignmentSpecifier> alignment_specs{};
    std::uint64_t resolved_alignment = 0;

    // VarDecl, scalar/struct/class (any non-reference, non-pointer)
    // only: true for `const T name = expr;`/`const ClassName name{args    };

    // -- an immutable local, initialized exactly once at declaration and
    // rejected by movecheck (its own MirStatementKind::Assign case) on
    // any subsequent reassignment attempt. Distinct from `const T&`/
    // `const T*` (a shared borrow/read-only pointer, ch05.2/§5.7): those
    // already track their own read-only-ness via Type::is_mutable_ref/
    // is_mutable_pointee and never set this flag (see parse_var_decl).
    // Always false for a Reference/Pointer-typed `type` above.
    bool is_const = false;
    // True for a local declared `constexpr` -- syntactically distinct from
    // `const`, but equally immutable once initialized. Phase A only records
    // the spelling; constant-evaluation semantics land later.
    bool is_constexpr = false;
    // True only for a block-scope `static` variable declaration: same
    // lexical scope as an ordinary local, but static storage duration and
    // exactly-once initialization semantics at runtime.
    bool is_static_local = false;

    // VarDecl, class-typed only (ch04 §4.2 / spec §6.1):
    // `ClassName name{args};`, direct-initialization via an explicit
    // constructor call -- mutually exclusive with `init` above (a class
    // type has no `=`-initializer form in this version, only this
    // brace-args form or a bare, zero-initialized declaration calling no
    // constructor at all). `has_ctor_args` is needed to tell an
    // explicit-but-empty call (`ClassName name{};`) apart from no call at
    // all (a bare `ClassName name;`) -- `ctor_args` alone being empty
    // can't distinguish those two.
    bool has_ctor_args = false;
    std::vector<ExprPtr> ctor_args{};

    // Return / ExprStmt (value/expr)
    ExprPtr expr{};

    // If / While / Switch
    ExprPtr condition{};
    IfMode if_mode = IfMode::Runtime;
    StmtPtr then_branch{};
    StmtPtr else_branch{}; // optional, If only
    std::vector<SwitchCase> switch_cases{};

    // Block
    std::vector<StmtPtr> statements{};
    // Block: true for an `unsafe { }` block (ch01 §1.3), false for an
    // ordinary `{ }`. An unsafe block is otherwise a completely normal
    // Block -- same lexical scoping, same statement list -- this flag
    // only tells the move checker to relax the specific ch05.5 checks
    // it's licensed to relax (raw pointer dereference, calling an
    // `extern "C"` function), and tells codegen to skip its own runtime
    // checks (span bounds, integer overflow -- ch05 §5.8/ch08 Q1), for
    // the statements directly and transitively nested inside it; every
    // other check (ch05.1-5.4) keeps running unconditionally regardless
    // of this flag -- every function is checked by default now (ch01),
    // so this is the *only* way any of ch05.5's operations ever becomes
    // legal, anywhere. Meaningless for every other StmtKind.
    bool is_unsafe = false;
};

[[nodiscard]] inline Param deep_clone_param(const Param& param);
[[nodiscard]] inline ExprPtr deep_clone_expr(const Expr& expr);
[[nodiscard]] inline StmtPtr deep_clone_stmt(const Stmt& stmt);
inline void assign_expr_fields(Expr& dest, const Expr& src);
inline void assign_stmt_fields(Stmt& dest, const Stmt& src);

inline void rewrite_expr_locs(Expr& expr, const SourceLocation& loc) {
    expr.loc = loc;
    if (expr.lhs != nullptr) rewrite_expr_locs(*expr.lhs, loc);
    if (expr.rhs != nullptr) rewrite_expr_locs(*expr.rhs, loc);
    if (expr.third != nullptr) rewrite_expr_locs(*expr.third, loc);
    for (std::size_t i = 0; i < expr.args.size(); i++) rewrite_expr_locs(*expr.args[i], loc);
    for (std::size_t i = 0; i < expr.explicit_template_args.size(); i++) {
        ExplicitTemplateArg& arg = expr.explicit_template_args[i];
        if (!arg.is_type && arg.value != nullptr) rewrite_expr_locs(*arg.value, loc);
    }
    for (std::size_t i = 0; i < expr.lambda_captures.size(); i++) {
        LambdaCapture& capture = expr.lambda_captures[i];
        if (capture.init != nullptr) rewrite_expr_locs(*capture.init, loc);
    }
    for (std::size_t i = 0; i < expr.lambda_params.size(); i++) {
        Param& param = expr.lambda_params[i];
        if (param.default_expr != nullptr) rewrite_expr_locs(*param.default_expr, loc);
    }
    if (expr.lambda_body != nullptr) {
        auto rewrite_stmt_locs = [&](auto&& self, Stmt& stmt) -> void {
            stmt.loc = loc;
            if (stmt.init != nullptr) rewrite_expr_locs(*stmt.init, loc);
            for (std::size_t i = 0; i < stmt.ctor_args.size(); i++) rewrite_expr_locs(*stmt.ctor_args[i], loc);
            if (stmt.expr != nullptr) rewrite_expr_locs(*stmt.expr, loc);
            if (stmt.condition != nullptr) rewrite_expr_locs(*stmt.condition, loc);
            if (stmt.then_branch != nullptr) self(self, *stmt.then_branch);
            if (stmt.else_branch != nullptr) self(self, *stmt.else_branch);
            for (std::size_t i = 0; i < stmt.switch_cases.size(); i++) {
                SwitchCase& switch_case = stmt.switch_cases[i];
                switch_case.loc = loc;
                if (switch_case.value != nullptr) rewrite_expr_locs(*switch_case.value, loc);
                for (std::size_t j = 0; j < switch_case.statements.size(); j++) self(self, *switch_case.statements[j]);
            }
            for (std::size_t i = 0; i < stmt.statements.size(); i++) self(self, *stmt.statements[i]);
        };
        rewrite_stmt_locs(rewrite_stmt_locs, *expr.lambda_body);
    }
}

[[nodiscard]] inline Param deep_clone_param(const Param& param) {
    return Param{param};
}

inline Param::Param(const Param& other)
    : type{other.type},
      name{other.name},
      resolved_local{other.resolved_local},
      lifetime{other.lifetime},
      default_expr{},
      generic_concept{other.generic_concept},
      require_thread_movable{other.require_thread_movable},
      require_thread_shareable{other.require_thread_shareable},
      is_parameter_pack{other.is_parameter_pack} {
    if (other.default_expr != nullptr) {
        ExprPtr cloned_default_expr = deep_clone_expr(*other.default_expr);
        default_expr = std::shared_ptr<Expr>{cloned_default_expr.release()};
    }
}

inline Param& Param::operator=(const Param& other) {
    Param clone{other};
    *this = std::move(clone);
    return *this;
}

inline ExplicitTemplateArg::ExplicitTemplateArg(const ExplicitTemplateArg& other)
    : is_type{other.is_type}, type{other.type}, value{} {
    if (other.value != nullptr) {
        ExprPtr cloned_value_expr = deep_clone_expr(*other.value);
        value = std::shared_ptr<Expr>{cloned_value_expr.release()};
    }
}

inline ExplicitTemplateArg& ExplicitTemplateArg::operator=(const ExplicitTemplateArg& other) {
    ExplicitTemplateArg clone{other};
    *this = std::move(clone);
    return *this;
}

// The single authoritative field-by-field copy of an `Expr`.
//
// Every clone of an `Expr` anywhere in the compiler funnels through here:
// `deep_clone_expr`, `Expr`'s copy constructor and `Expr::operator=` are thin
// wrappers over it, and no other translation unit enumerates `Expr`'s fields.
// A field added to `Expr` but not listed below is a field silently dropped by
// every clone in the compiler -- that is exactly how `resolved_local` used to
// get lost -- so `tests/parser_test.cpp` carries a structured-binding guard
// that turns the omission into a compile error naming `Expr` and its field
// count. If you add a field to `Expr`, add it here and to that guard.
//
// `dest` must be a freshly constructed node (or at least not alias `src`);
// `Expr::operator=` copies-then-moves precisely so that self-assignment can
// never reach this function.
inline void assign_expr_fields(Expr& dest, const Expr& src) {
    dest.kind = src.kind;
    dest.resolved_local = src.resolved_local;
    dest.loc = src.loc;
    dest.int_value = src.int_value;
    dest.float_value = src.float_value;
    dest.bool_value = src.bool_value;
    dest.name = src.name;
    dest.explicit_global_qualification = src.explicit_global_qualification;
    dest.binary_op = src.binary_op;
    dest.lhs = nullptr;
    if (src.lhs != nullptr) dest.lhs = deep_clone_expr(*src.lhs);
    dest.rhs = nullptr;
    if (src.rhs != nullptr) dest.rhs = deep_clone_expr(*src.rhs);
    dest.third = nullptr;
    if (src.third != nullptr) dest.third = deep_clone_expr(*src.third);
    dest.fold_ellipsis_on_left = src.fold_ellipsis_on_left;
    dest.unary_op = src.unary_op;
    dest.args.clear();
    for (std::size_t i = 0; i < src.args.size(); i++) dest.args.push_back(deep_clone_expr(*src.args[i]));
    // `ExplicitTemplateArg`, `Type`, `LambdaCapture` and `Param` all have deep
    // copy constructors of their own, so plain assignment already deep-clones
    // whatever `Expr` nodes they hold.
    dest.explicit_template_args = src.explicit_template_args;
    dest.type = src.type;
    dest.sizeof_operand_is_type = src.sizeof_operand_is_type;
    dest.has_paren_init = src.has_paren_init;
    dest.destroy_through_pointer = src.destroy_through_pointer;
    dest.through_arrow = src.through_arrow;
    dest.implicit_arrow_deref = src.implicit_arrow_deref;
    dest.implicit_arrow_chain_safe = src.implicit_arrow_chain_safe;
    dest.lambda_captures = src.lambda_captures;
    dest.lambda_blanket_mode = src.lambda_blanket_mode;
    dest.lambda_params = src.lambda_params;
    dest.has_lambda_explicit_return_type = src.has_lambda_explicit_return_type;
    dest.lambda_is_mutable = src.lambda_is_mutable;
    dest.lambda_body = nullptr;
    if (src.lambda_body != nullptr) dest.lambda_body = deep_clone_stmt(*src.lambda_body);
}

// The single authoritative field-by-field copy of a `Stmt`; see
// `assign_expr_fields` above for the contract and for why there is only one.
inline void assign_stmt_fields(Stmt& dest, const Stmt& src) {
    dest.kind = src.kind;
    dest.loc = src.loc;
    dest.type = src.type;
    dest.var_name = src.var_name;
    dest.declared_local = src.declared_local;
    dest.init = nullptr;
    if (src.init != nullptr) dest.init = deep_clone_expr(*src.init);
    dest.alignment_specs = src.alignment_specs;
    dest.resolved_alignment = src.resolved_alignment;
    dest.is_const = src.is_const;
    dest.is_constexpr = src.is_constexpr;
    dest.is_static_local = src.is_static_local;
    dest.has_ctor_args = src.has_ctor_args;
    dest.ctor_args.clear();
    for (std::size_t i = 0; i < src.ctor_args.size(); i++) dest.ctor_args.push_back(deep_clone_expr(*src.ctor_args[i]));
    dest.expr = nullptr;
    if (src.expr != nullptr) dest.expr = deep_clone_expr(*src.expr);
    dest.condition = nullptr;
    if (src.condition != nullptr) dest.condition = deep_clone_expr(*src.condition);
    dest.if_mode = src.if_mode;
    dest.then_branch = nullptr;
    if (src.then_branch != nullptr) dest.then_branch = deep_clone_stmt(*src.then_branch);
    dest.else_branch = nullptr;
    if (src.else_branch != nullptr) dest.else_branch = deep_clone_stmt(*src.else_branch);
    // `SwitchCase` has a deep copy constructor of its own.
    dest.switch_cases = src.switch_cases;
    dest.statements.clear();
    for (std::size_t i = 0; i < src.statements.size(); i++) dest.statements.push_back(deep_clone_stmt(*src.statements[i]));
    dest.is_unsafe = src.is_unsafe;
}

[[nodiscard]] inline ExprPtr deep_clone_expr(const Expr& expr) {
    auto clone = std::make_unique<Expr>();
    assign_expr_fields(*clone, expr);
    return clone;
}

[[nodiscard]] inline StmtPtr deep_clone_stmt(const Stmt& stmt) {
    auto clone = std::make_unique<Stmt>();
    assign_stmt_fields(*clone, stmt);
    return clone;
}

// The single authoritative mapping from a `LambdaCapture` to the `Identifier`
// expression that names the captured entity in the *enclosing* scope. Codegen
// materializes each capture by evaluating such a node, and a bare name would
// not resolve: the node has to carry the enclosing declaration's id, which is
// exactly the sort of field that used to get dropped by hand-written synthesis.
// Anything that builds one of these must go through here.
[[nodiscard]] inline Expr make_capture_identifier(const LambdaCapture& capture, const SourceLocation& loc) {
    Expr ident{};
    ident.kind = ExprKind::Identifier;
    ident.loc = loc;
    ident.name = capture.name;
    ident.resolved_local = capture.resolved_local;
    return ident;
}

[[nodiscard]] inline ExprPtr deep_clone_expr_with_loc(const Expr& expr, const SourceLocation& loc) {
    ExprPtr clone = deep_clone_expr(expr);
    rewrite_expr_locs(*clone, loc);
    return clone;
}

class GlobalVar {
  public:
    virtual ~GlobalVar() = default;
    GlobalVar() = default;
    GlobalVar(const GlobalVar& other);
    GlobalVar& operator=(const GlobalVar& other);
    GlobalVar(GlobalVar&&) = default;
    GlobalVar& operator=(GlobalVar&&) = default;

    StmtPtr decl;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    std::string owning_module;
};

class Initializer {
  public:
    virtual ~Initializer() = default;
    // `= expr`
    ExprPtr expr;
    // `{}` / `{args...}`
    bool has_brace_args = false;
    std::vector<ExprPtr> brace_args;

    Initializer() = default;
    Initializer(const Initializer& other);
    Initializer& operator=(const Initializer& other);
    Initializer(Initializer&&) = default;
    Initializer& operator=(Initializer&&) = default;
};

class MemberInitializer {
  public:
    virtual ~MemberInitializer() = default;
    std::string member_name;
    Initializer initializer;
    SourceLocation loc;

    MemberInitializer() = default;
    MemberInitializer(const MemberInitializer&) = default;
    MemberInitializer& operator=(const MemberInitializer&) = default;
    MemberInitializer(MemberInitializer&&) = default;
    MemberInitializer& operator=(MemberInitializer&&) = default;
};


inline Type::Type(const Type& other)
    : kind{other.kind},
      lifetime{other.lifetime},
      name{other.name},
      pointee{},
      element{},
      array_size{other.array_size},
      array_size_expr{},
      function_return{},
      function_params{other.function_params},
      is_unsafe_function_pointer{other.is_unsafe_function_pointer},
      is_const_function{other.is_const_function},
      function_ref_qualifier{other.function_ref_qualifier},
      is_mutable_ref{other.is_mutable_ref},
      is_rvalue_ref{other.is_rvalue_ref},
      is_mutable_pointee{other.is_mutable_pointee},
      is_const_qualified{other.is_const_qualified},
      is_reference_wrapper_lifetime_source{other.is_reference_wrapper_lifetime_source},
      template_args{other.template_args},
      non_type_args{},
      is_pack_expansion{other.is_pack_expansion} {
    if (other.pointee != nullptr) {
        Type copied_pointee{*other.pointee};
        pointee = std::make_shared<Type>(std::move(copied_pointee));
    }
    if (other.element != nullptr) {
        Type copied_element{*other.element};
        element = std::make_shared<Type>(std::move(copied_element));
    }
    if (other.array_size_expr != nullptr) {
        ExprPtr cloned_array_size_expr = deep_clone_expr(*other.array_size_expr);
        array_size_expr = std::shared_ptr<Expr>{cloned_array_size_expr.release()};
    }
    if (other.function_return != nullptr) {
        Type copied_function_return{*other.function_return};
        function_return = std::make_shared<Type>(std::move(copied_function_return));
    }
    for (std::size_t i = 0; i < other.non_type_args.size(); i++) {
        if (other.non_type_args[i] != nullptr) {
            ExprPtr cloned_non_type_arg = deep_clone_expr(*other.non_type_args[i]);
            non_type_args.push_back(std::shared_ptr<Expr>{cloned_non_type_arg.release()});
        } else {
            non_type_args.push_back(nullptr);
        }
    }
}

inline Type& Type::operator=(const Type& other) {
    Type clone{other};
    *this = std::move(clone);
    return *this;
}

[[nodiscard]] inline GlobalVar clone_global_var(const GlobalVar& global) {
    GlobalVar clone{};
    if (global.decl != nullptr) clone.decl = deep_clone_stmt(*global.decl);
    clone.namespace_path = global.namespace_path;
    clone.is_exported = global.is_exported;
    clone.owning_module = global.owning_module;
    return clone;
}

inline AlignmentSpecifier::AlignmentSpecifier(const AlignmentSpecifier& other)
    : loc{other.loc}, operand_is_type{other.operand_is_type}, type{other.type}, expr{} {
    if (other.expr != nullptr) expr = deep_clone_expr(*other.expr);
}

inline AlignmentSpecifier& AlignmentSpecifier::operator=(const AlignmentSpecifier& other) {
    AlignmentSpecifier clone{other};
    *this = std::move(clone);
    return *this;
}

inline GlobalVar::GlobalVar(const GlobalVar& other)
    : decl{}, namespace_path{other.namespace_path}, is_exported{other.is_exported}, owning_module{other.owning_module} {
    if (other.decl != nullptr) decl = deep_clone_stmt(*other.decl);
}

inline GlobalVar& GlobalVar::operator=(const GlobalVar& other) {
    GlobalVar clone{other};
    *this = std::move(clone);
    return *this;
}

inline LambdaCapture::LambdaCapture(const LambdaCapture& other)
    : name{other.name}, by_reference{other.by_reference}, init{}, resolved_local{other.resolved_local} {
    if (other.init != nullptr) init = deep_clone_expr(*other.init);
}

inline LambdaCapture& LambdaCapture::operator=(const LambdaCapture& other) {
    LambdaCapture clone{other};
    *this = std::move(clone);
    return *this;
}

inline Initializer::Initializer(const Initializer& other) : expr{}, has_brace_args{other.has_brace_args}, brace_args{} {
    if (other.expr != nullptr) expr = deep_clone_expr(*other.expr);
    for (std::size_t i = 0; i < other.brace_args.size(); i++) brace_args.push_back(deep_clone_expr(*other.brace_args[i]));
}

inline Initializer& Initializer::operator=(const Initializer& other) {
    Initializer clone{other};
    *this = std::move(clone);
    return *this;
}

inline SwitchCase::SwitchCase(const SwitchCase& other) : loc{other.loc}, value{}, statements{} {
    if (other.value != nullptr) this->value = deep_clone_expr(*other.value);
    for (std::size_t i = 0; i < other.statements.size(); i++) this->statements.push_back(deep_clone_stmt(*other.statements[i]));
}

inline SwitchCase& SwitchCase::operator=(const SwitchCase& other) {
    SwitchCase clone{other};
    *this = std::move(clone);
    return *this;
}

// Every member of `Expr` has an in-class default member initializer, so this
// constructor needs no member-initializer-list of its own and the field list
// lives in exactly one place: `assign_expr_fields`.
inline Expr::Expr(const Expr& other) {
    assign_expr_fields(*this, other);
}

inline Expr& Expr::operator=(const Expr& other) {
    Expr clone{other};
    *this = std::move(clone);
    return *this;
}

// See `Expr`'s copy constructor above: the field list lives only in
// `assign_stmt_fields`.
inline Stmt::Stmt(const Stmt& other) {
    assign_stmt_fields(*this, other);
}

inline Stmt& Stmt::operator=(const Stmt& other) {
    Stmt clone{other};
    *this = std::move(clone);
    return *this;
}


// ch05 §5.14: a generic type's (class or struct) own template
// parameter -- either a *type* parameter (`typename T`, bare --
// `concept_name` empty -- or `ConceptName T`, constrained), or a
// *non-type* parameter (`size_t Idx`, restricted to scalar types --
// `is_non_type`/`non_type_type`, only used by a variadic class-template
// specialization's own header, e.g. `TupleImpl<Idx, Head, Tail...>`'s
// `Idx`). `is_pack` marks the abbreviated-pack form (`typename... Ts`),
// legal only as the *last* parameter in a variadic primary template's
// own header (`template<typename... Ts> class Tuple;`) -- see
// ClassDef::is_variadic_primary_template. A `struct`'s own parameter
// can never be bare (triviality, ch04 §4.1, is a whole-type property no
// per-member clause could decompose the way a class's methods can --
// see Function::method_requires_concept) -- enforced by the parser,
// not represented as a separate flag here. ch05 §5.11: also reused for
// a full-header-form generic *function*'s own template parameter (see
// Function::template_params) -- the exact same shape (bare/constrained
// type parameter, or non-type parameter) applies identically there.
class GenericTypeParam {
  public:
    virtual ~GenericTypeParam() = default;
    GenericTypeParam() = default;
    GenericTypeParam(const GenericTypeParam&) = default;
    GenericTypeParam& operator=(const GenericTypeParam&) = default;
    GenericTypeParam(GenericTypeParam&&) = default;
    GenericTypeParam& operator=(GenericTypeParam&&) = default;

    std::string name;
    std::string concept_name; // empty = bare (type parameter only)
    bool is_pack = false;
    bool is_non_type = false;
    Type non_type_type; // meaningful only when is_non_type is true
};

enum class AccessSpecifier {
    Public,
    Private,
};

enum class BaseClassKind {
    Unknown,
    OrdinaryClass,
    Interface,
};

class BaseSpecifier {
  public:
    virtual ~BaseSpecifier() = default;
    BaseSpecifier() = default;
    BaseSpecifier(const BaseSpecifier&) = default;
    BaseSpecifier& operator=(const BaseSpecifier&) = default;
    BaseSpecifier(BaseSpecifier&&) = default;
    BaseSpecifier& operator=(BaseSpecifier&&) = default;

    Type base_type;
    AccessSpecifier access = AccessSpecifier::Private;
    bool is_virtual = false;
    BaseClassKind kind = BaseClassKind::Unknown;
    // ch05 §5.14: meaningful for a variadic specialization's recursive
    // base-clause shape (e.g. the trailing `Tail...` in
    // `Tuple<Tail...>`). Empty for every other base-specifier.
    std::string pack_arg_name;
};

class ClassUsingDeclaration {
  public:
    virtual ~ClassUsingDeclaration() = default;
    ClassUsingDeclaration() = default;
    ClassUsingDeclaration(std::string base_name, std::string member_name, AccessSpecifier access)
        : base_name{std::move(base_name)}, member_name{std::move(member_name)}, access{access} {}
    ClassUsingDeclaration(const ClassUsingDeclaration&) = default;
    ClassUsingDeclaration& operator=(const ClassUsingDeclaration&) = default;

    std::string base_name;
    std::string member_name;
    AccessSpecifier access = AccessSpecifier::Private;
};

class TypeAliasDecl {
  public:
    virtual ~TypeAliasDecl() = default;
    TypeAliasDecl() = default;
    TypeAliasDecl(const TypeAliasDecl&) = default;
    TypeAliasDecl& operator=(const TypeAliasDecl&) = default;

    SourceLocation loc;
    Type underlying_type;
    std::string name;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    std::string owning_module;
};

class Function {
  public:
    virtual ~Function() = default;
    Function() = default;
    Function(const Function& other);
    Function& operator=(const Function& other);
    Function(Function&&) = default;
    Function& operator=(Function&&) = default;

    Type return_type;
    std::string name;
    // Where this function's declaration begins -- same purpose as
    // Expr::loc/Stmt::loc, for diagnostics that are about the function
    // itself (e.g. "function 'f' cannot return class 'X' by value")
    // rather than a specific statement/expression inside it.
    SourceLocation loc;
    std::vector<Param> params;
    LifetimeAnnotation return_lifetime;
    // Null for a bodyless `extern "C"` declaration (ch02 §2.1) or a bare
    // `extern` module-linkage declaration (ch11 §11.6) -- defined
    // elsewhere, linked in externally. Always non-null for every other
    // function (an ordinary definition, or an `extern "C"` *definition*
    // with a body). Nothing outside parsing/movecheck/codegen's
    // extern-declaration handling should assume this is always non-null.
    StmtPtr body;
    // ch02 §2.1: requests C linkage. A bodyless `extern "C"` declaration
    // is always implicitly unchecked (no scpp compiler ever sees its
    // real implementation), so calling it always requires
    // `[[scpp::unsafe]] { }` (ch01/ch05 §5.5) -- the *only* remaining
    // always-unchecked callee category, now that every ordinary
    // function is checked by default (ch01 §1.3). An `extern "C"`
    // *definition* (body non-null) is an ordinary, fully-checked
    // function that additionally requests C linkage -- calling it needs
    // no `[[scpp::unsafe]] { }` at all (unless it's also itself marked
    // `is_unsafe` below).
    bool is_extern_c = false;
    // ch11 §11.6: a bare `extern` (no `"C"` string) bodyless declaration
    // -- ordinary scpp linkage; calling it needs no
    // `[[scpp::unsafe]] { }` either (the module's own author is trusted
    // to check the real implementation elsewhere, see §11.6's own
    // reasoning). Mutually exclusive with is_extern_c (either this
    // function requests C ABI, or ordinary scpp linkage, never both).
    bool is_module_extern = false;
    // ch01 §1.2/§1.3: the function-level `[[scpp::unsafe]]` marker --
    // an attribute-specifier-seq containing the attribute-token
    // `unsafe` appertaining to this function's own declaration (leading
    // position, before the return type), as opposed to a *nested*
    // `[[scpp::unsafe]] { }` block somewhere inside an otherwise-
    // ordinary body (that form needs no AST field of its own: it's just
    // an ordinary Stmt::is_unsafe=true Block, indistinguishable from any
    // other unsafe block once parsed). Two effects follow, both handled
    // by movecheck (see check_function's own entry_state setup and
    // apply_expr's Call case): the function's *entire* body becomes an
    // unsafe context throughout (as if its whole body were itself
    // wrapped in one `[[scpp::unsafe]] { }`), and calling this function
    // from anywhere becomes one more of ch05 §5.5's gated operations --
    // scpp's equivalent of Rust's `unsafe fn`, for a function whose
    // soundness depends on a precondition only its caller can guarantee.
    // If this function is declared more than once (e.g. a bare `extern`
    // forward declaration later defined elsewhere), every declaration
    // must repeat this attribute consistently (ch01 §1.3 (2) in the
    // formal spec) -- enforced by movecheck, not the parser (which
    // parses one declaration at a time and has no cross-declaration
    // view).
    bool is_unsafe = false;
    bool is_nodiscard = false;
    std::string nodiscard_reason;
    // True only for a non-exported definition recovered from a compiled
    // module's structured compile-time payload. Lets the importer keep
    // reachable private helper bodies available for generic/constexpr use
    // without pretending they were part of the module's ordinary exported
    // surface.
    bool is_compile_time_dependency = false;
    // True for an imported hidden helper body that must stay available so
    // downstream codegen/generic instantiation can inline or clone it, but
    // whose defining module has already movechecked it once. Importers skip
    // re-running movecheck on such runtime-only helper bodies.
    bool skip_imported_body_verification = false;
    // Records whether this declaration was spelled `constexpr` or
    // `consteval`. RuntimeOnly is the ordinary pre-existing case.
    FunctionEvalMode eval_mode = FunctionEvalMode::RuntimeOnly;
    // ch02 §2.1: the declaration ends in a trailing `...` (e.g.
    // `printf(const char* fmt, ...)`). Parsed and stored, but v0.1
    // doesn't yet support a *call site* passing extra arguments beyond
    // `params` to such a function (see codegen's declare_function) --
    // only parsing/declaring the correct variadic signature shape is
    // implemented in this first slice, per the spec's own scoping.
    bool has_varargs = false;

    // ch05 §5.14: non-empty only for a method (including a constructor)
    // of a generic `class`/`struct` that carries its own `requires
    // Concept<T>` clause (e.g. `bool less_than(const T& o) const requires
    // std::totally_ordered<T> { ... }`) -- names the concept constraining
    // the *enclosing generic type's own* type parameter, for *this one
    // method's own body-check only* (ch05 §5.11's "concept is optional,
    // decomposed per member" principle, applied to a class instead of a
    // whole function). Empty for a method with no such clause (the
    // parameter stays fully opaque within that method's own body: move/
    // store/pass-through/return only, exactly like a bare generic-
    // function parameter). Meaningless outside a generic type's own
    // template definition (see ClassDef/StructDef::template_params).
    std::string method_requires_concept;

    // ch05 §5.11: true when at least one parameter has a non-empty
    // Param::generic_concept -- this is the generic function's own
    // *template* definition, checked once abstractly against each
    // constrained parameter's witness class, never emitted to codegen
    // directly (see Codegen::generate, which skips every
    // is_generic_template Function entirely). Each concrete call site
    // instead gets a separate monomorphized clone (an ordinary,
    // non-template Function, injected into Program::functions by the
    // concept-monomorphization pass) with its own distinct mangled name.
    bool is_generic_template = false;

    // ch05 §5.11/§5.14: non-empty only for a generic *function* spelled
    // with the full `template<...>` header form (as opposed to the
    // abbreviated `Concept auto` form, whose constrained parameters are
    // tracked per-Param via Param::generic_concept instead) -- e.g.
    // `template<size_t I, typename Head, typename... Tail> Head&
    // get(TupleImpl<I, Head, Tail...>& t) { ... }`. Real C++ treats the
    // two spellings as fully equivalent (ch05 §5.11); the full form
    // additionally allows a type parameter with *no* corresponding
    // function-parameter position at all (a "return-type-only" generic,
    // e.g. `template<typename T> T make();`), which must be supplied
    // explicitly at the call site (Expr::explicit_template_args) since
    // there is nothing to deduce it from. Never a pack for a function
    // (only a generic *type*'s own header supports one, ch05 §5.14) --
    // parser-enforced. This function is a *template* exactly like an
    // is_generic_template one (never emitted to codegen directly; each
    // concrete call site gets a separate monomorphized clone) --
    // is_generic_template is also set to true whenever this is non-empty,
    // so every existing "is this a template" check keeps working
    // unchanged.
    std::vector<GenericTypeParam> template_params;

    // ch05 §5.14: non-empty only on a method/constructor/destructor still
    // attached to a generic class template definition (including an
    // ordinary partial specialization pattern) rather than a concrete
    // instantiation. Distinguishes otherwise same-named exposed template
    // definitions that all synthesize methods against the same spelled
    // class name (e.g. `function_...`) so movecheck can recover exactly
    // which one owns this method without relying on that unstable name
    // alone. Cleared again on every concrete clone and on every
    // non-template class method.
    std::string generic_method_owner_id;
    // Member functions only: the owning class's own fully-qualified name.
    // Empty for a free function.
    std::string member_owner_class;
    // Non-empty only for a lambda's synthesized closure-class `_call`
    // method (see monomorphize.cppm's resolve_lambda): the *lexically*
    // enclosing class whose private members this lambda's body may
    // access, as if the lambda's body appeared directly at the point
    // where the lambda-expression itself is written -- exactly how real
    // C++ access control treats a closure type ([expr.prim.lambda]).
    // This is deliberately kept separate from member_owner_class (which
    // for a `_call` method names the closure's own, unrelated synthetic
    // class, needed for `this`-typing/method registration) rather than
    // overloading that field, so nothing else that inspects
    // member_owner_class (overload resolution, declared_members_of,
    // and so on) is affected; only movecheck's own private-access-
    // checking perspective (DataflowState::current_class, set from this
    // field in preference to member_owner_class when non-empty) reads
    // it. Empty for every ordinary function/method, preserving today's
    // behavior unchanged.
    std::string access_context_class;
    // Constructors only: `Ctor(...) : Base{...}, field{...}, other{...}
    // { ... }` parsed exactly as written (still in source order). Entries
    // may name the direct base class itself or a direct field. Codegen
    // still applies the direct base first and then fields in declaration
    // order, matching real C++'s construction-order rule rather than the
    // list's textual order.
    std::vector<MemberInitializer> member_initializers;
    // Member functions only: trailing ref-qualifier after the parameter
    // list (`&` / `&&`). `None` means unqualified, so the method is
    // callable on either an lvalue or rvalue receiver. `const` remains
    // represented by params[0]'s own `this` type.
    ReceiverRefQualifier receiver_ref_qualifier = ReceiverRefQualifier::None;
    // Member functions only: whether this declaration was spelled
    // `static`. A static member function has no implicit `this`
    // parameter and is called through `ClassName::method(...)`.
    bool is_static = false;
    // Member functions only: whether this declaration was parsed under a
    // `public:` or `private:` section of its enclosing class. Free
    // functions leave this at the default Public.
    AccessSpecifier access = AccessSpecifier::Public;
    // Member functions only: whether the declaration is marked `virtual`.
    bool is_virtual = false;
    // Member functions only: whether the declaration is marked `override`.
    bool is_override = false;
    // Member functions only: whether the declaration ends with a pure-
    // specifier (`= 0`).
    bool is_pure = false;
    // Special-member functions only: whether the declaration is defaulted
    // (`= default`).
    bool is_defaulted = false;
    // Member declarations only: parsed in the current translation unit as
    // a declaration-without-body that expects a later out-of-line
    // definition. Cleared again on imported/cloned declarations and once a
    // matching definition is merged in.
    bool expects_out_of_line_definition = false;

    // ch05 §5.14: non-empty only for a synthesized *forwarding stub* --
    // a derived class inheriting a base method it doesn't itself
    // override (e.g. "Derived_foo" forwarding to "Base_foo") -- names
    // the real function this one's own call should be redirected to at
    // codegen (Codegen::define_forwarding_function). `body` is always
    // null for one of these: there is no scpp-level AST to move/borrow-
    // check at all (movecheck already skips every bodyless function,
    // the same as an `extern` declaration), since forwarding a
    // *pointer* unchanged (this class's own flattened layout, see
    // ClassDef::base_specifiers/direct_ordinary_base, makes a derived instance's leading
    // bytes already byte-identical to its base) needs no scpp-level
    // logic of its own -- purely a thin, codegen-only wrapper. Avoids a
    // real scpp-level upcast/base-conversion expression, which doesn't
    // exist yet (this is the only place a derived-to-base "conversion"
    // is needed in v0.1).
    std::string forwards_to;

    // ch11 §11.4/§11.5: the namespace path this declaration lexically
    // lives in, e.g. `namespace std { ... }` -> {"std"}, `namespace
    // a::b { ... }` -> {"a", "b"}. Empty for a declaration at file/global
    // scope (today's default, unaffected by any of this). `name` itself
    // already carries the fully-qualified form (e.g. "std::string_new")
    // -- namespace_path is tracked *separately* so the export/namespace
    // validation pass (§11.5) and codegen's mangling scheme (§11.9) can
    // check/encode namespace segments individually, not just as one
    // opaque joined string.
    std::vector<std::string> namespace_path;
    // ch11 §11.3: true for an `export`-prefixed declaration (or one
    // inside an `export { ... }` group, or a synthesized method of an
    // `export class`/`export struct`). Only actually exports if
    // namespace_path also starts with the enclosing module's own dotted
    // name -- see the export/namespace validation pass. Meaningless
    // (never consulted) when the enclosing Program isn't a module at
    // all (module_name empty).
    bool is_exported = false;
    // ch11 §11.8/§11.9: empty for a declaration belonging to the
    // Program currently being compiled (whether or not that Program is
    // itself a module -- see Program::module_name); set to the imported
    // module's own dotted name when this Function was recovered from an
    // imported module's interface and merged in (see the driver's
    // cross-module signature recovery) -- codegen's mangling scheme
    // keys off this field, not Program::module_name, so a merged-in
    // declaration is mangled exactly the way its owning module's own
    // separate compilation will define it.
    std::string owning_module;
    // Module whose full private/exported visibility should be used when
    // checking/instantiating this function's body. Usually identical to
    // owning_module; preserved separately for locally-instantiated clones
    // of imported generics, which may need local symbol ownership while
    // still seeing the defining module's hidden helpers.
    std::string visibility_module;
    // Locations of any standalone `Type f(...);` ordinary forward
    // declaration(s) (ch05 §5.x) that reconcile_ordinary_forward_
    // declarations (parser.cppm) matched to *this* definition and then
    // discarded from Program::functions, keeping only this merged
    // definition. The driver's own module-interface writer needs these
    // back: once it strips this definition's body down to a bare
    // declaration too (to build a private-body-free `.scppm`), the
    // original standalone declaration's own source text becomes an
    // exact, redundant second copy of the very same declaration -- which
    // a later `import` of that interface would then reject outright
    // (ch05 §5.10 forbids re-declaring an identical signature more than
    // once). Empty for every function that was never split across a
    // separate forward declaration to begin with.
    std::vector<SourceLocation> superseded_forward_declaration_locs;
};

[[nodiscard]] inline bool is_special_member_this_param(const Type& type, std::string_view owner_name) {
    if (type.kind != TypeKind::Reference || !type.is_mutable_ref || type.pointee == nullptr ||
        type.pointee->kind != TypeKind::Named) {
        return false;
    }
    std::string_view spelled_name{type.pointee->name};
    std::size_t spelled_scope = spelled_name.rfind("::");
    std::size_t owner_scope = owner_name.rfind("::");
    std::string_view spelled_unqualified =
        spelled_scope == static_cast<std::size_t>(-1) ? spelled_name : spelled_name.substr(spelled_scope + 2);
    std::string_view owner_unqualified =
        owner_scope == static_cast<std::size_t>(-1) ? owner_name : owner_name.substr(owner_scope + 2);
    if (spelled_name == owner_name || spelled_unqualified == owner_unqualified) return true;
    std::size_t spelled_start = static_cast<std::size_t>(0);
    if (spelled_scope != static_cast<std::size_t>(-1)) spelled_start = spelled_scope + 2;
    std::string_view spelled_tail = spelled_name.substr(spelled_start);
    std::size_t spelled_dot = spelled_tail.rfind(".");
    std::string spelled_base{};
    if (spelled_dot == static_cast<std::size_t>(-1)) {
        spelled_base = std::string(spelled_tail.data(), spelled_tail.size());
    } else {
        spelled_base = std::string(spelled_tail.data(), spelled_dot);
    }
    std::size_t owner_start = static_cast<std::size_t>(0);
    if (owner_scope != static_cast<std::size_t>(-1)) owner_start = owner_scope + 2;
    std::string_view owner_tail = owner_name.substr(owner_start);
    std::size_t owner_dot = owner_tail.rfind(".");
    std::string owner_base{};
    if (owner_dot == static_cast<std::size_t>(-1)) {
        owner_base = std::string(owner_tail.data(), owner_tail.size());
    } else {
        owner_base = std::string(owner_tail.data(), owner_dot);
    }
    return spelled_base == owner_base;
}

[[nodiscard]] inline bool is_special_member_const_lvalue_self_param(const Type& type, std::string_view owner_name) {
    if (type.kind != TypeKind::Reference || type.is_rvalue_ref || type.is_mutable_ref || type.pointee == nullptr ||
        type.pointee->kind != TypeKind::Named) {
        return false;
    }
    std::string_view spelled_name{type.pointee->name};
    std::size_t spelled_scope = spelled_name.rfind("::");
    std::size_t owner_scope = owner_name.rfind("::");
    std::string_view spelled_unqualified =
        spelled_scope == static_cast<std::size_t>(-1) ? spelled_name : spelled_name.substr(spelled_scope + 2);
    std::string_view owner_unqualified =
        owner_scope == static_cast<std::size_t>(-1) ? owner_name : owner_name.substr(owner_scope + 2);
    if (spelled_name == owner_name || spelled_unqualified == owner_unqualified) return true;
    std::size_t spelled_start = static_cast<std::size_t>(0);
    if (spelled_scope != static_cast<std::size_t>(-1)) spelled_start = spelled_scope + 2;
    std::string_view spelled_tail = spelled_name.substr(spelled_start);
    std::size_t spelled_dot = spelled_tail.rfind(".");
    std::string spelled_base{};
    if (spelled_dot == static_cast<std::size_t>(-1)) {
        spelled_base = std::string(spelled_tail.data(), spelled_tail.size());
    } else {
        spelled_base = std::string(spelled_tail.data(), spelled_dot);
    }
    std::size_t owner_start = static_cast<std::size_t>(0);
    if (owner_scope != static_cast<std::size_t>(-1)) owner_start = owner_scope + 2;
    std::string_view owner_tail = owner_name.substr(owner_start);
    std::size_t owner_dot = owner_tail.rfind(".");
    std::string owner_base{};
    if (owner_dot == static_cast<std::size_t>(-1)) {
        owner_base = std::string(owner_tail.data(), owner_tail.size());
    } else {
        owner_base = std::string(owner_tail.data(), owner_dot);
    }
    return spelled_base == owner_base;
}

[[nodiscard]] inline bool is_special_member_rvalue_self_param(const Type& type, std::string_view owner_name) {
    if (type.kind != TypeKind::Reference || !type.is_rvalue_ref || type.pointee == nullptr ||
        type.pointee->kind != TypeKind::Named) {
        return false;
    }
    std::string_view spelled_name{type.pointee->name};
    std::size_t spelled_scope = spelled_name.rfind("::");
    std::size_t owner_scope = owner_name.rfind("::");
    std::string_view spelled_unqualified =
        spelled_scope == static_cast<std::size_t>(-1) ? spelled_name : spelled_name.substr(spelled_scope + 2);
    std::string_view owner_unqualified =
        owner_scope == static_cast<std::size_t>(-1) ? owner_name : owner_name.substr(owner_scope + 2);
    if (spelled_name == owner_name || spelled_unqualified == owner_unqualified) return true;
    std::size_t spelled_start = static_cast<std::size_t>(0);
    if (spelled_scope != static_cast<std::size_t>(-1)) spelled_start = spelled_scope + 2;
    std::string_view spelled_tail = spelled_name.substr(spelled_start);
    std::size_t spelled_dot = spelled_tail.rfind(".");
    std::string spelled_base{};
    if (spelled_dot == static_cast<std::size_t>(-1)) {
        spelled_base = std::string(spelled_tail.data(), spelled_tail.size());
    } else {
        spelled_base = std::string(spelled_tail.data(), spelled_dot);
    }
    std::size_t owner_start = static_cast<std::size_t>(0);
    if (owner_scope != static_cast<std::size_t>(-1)) owner_start = owner_scope + 2;
    std::string_view owner_tail = owner_name.substr(owner_start);
    std::size_t owner_dot = owner_tail.rfind(".");
    std::string owner_base{};
    if (owner_dot == static_cast<std::size_t>(-1)) {
        owner_base = std::string(owner_tail.data(), owner_tail.size());
    } else {
        owner_base = std::string(owner_tail.data(), owner_dot);
    }
    return spelled_base == owner_base;
}

[[nodiscard]] inline bool is_member_receiver_self_param(const Type& type, std::string_view owner_name) {
    if (type.kind != TypeKind::Reference || type.is_rvalue_ref || type.pointee == nullptr ||
        type.pointee->kind != TypeKind::Named) {
        return false;
    }
    std::string_view spelled_name{type.pointee->name};
    std::size_t spelled_scope = spelled_name.rfind("::");
    std::size_t owner_scope = owner_name.rfind("::");
    std::string_view spelled_unqualified =
        spelled_scope == static_cast<std::size_t>(-1) ? spelled_name : spelled_name.substr(spelled_scope + 2);
    std::string_view owner_unqualified =
        owner_scope == static_cast<std::size_t>(-1) ? owner_name : owner_name.substr(owner_scope + 2);
    if (spelled_name == owner_name || spelled_unqualified == owner_unqualified) return true;
    std::size_t spelled_start = static_cast<std::size_t>(0);
    if (spelled_scope != static_cast<std::size_t>(-1)) spelled_start = spelled_scope + 2;
    std::string_view spelled_tail = spelled_name.substr(spelled_start);
    std::size_t spelled_dot = spelled_tail.rfind(".");
    std::string spelled_base{};
    if (spelled_dot == static_cast<std::size_t>(-1)) {
        spelled_base = std::string(spelled_tail.data(), spelled_tail.size());
    } else {
        spelled_base = std::string(spelled_tail.data(), spelled_dot);
    }
    std::size_t owner_start = static_cast<std::size_t>(0);
    if (owner_scope != static_cast<std::size_t>(-1)) owner_start = owner_scope + 2;
    std::string_view owner_tail = owner_name.substr(owner_start);
    std::size_t owner_dot = owner_tail.rfind(".");
    std::string owner_base{};
    if (owner_dot == static_cast<std::size_t>(-1)) {
        owner_base = std::string(owner_tail.data(), owner_tail.size());
    } else {
        owner_base = std::string(owner_tail.data(), owner_dot);
    }
    return spelled_base == owner_base;
}

[[nodiscard]] inline bool is_constructor_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() == 0) return false;
    std::string_view name_view{fn.name};
    std::string_view new_suffix{"_new"};
    if (name_view.size() < new_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - new_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == new_suffix)) return false;
    return is_special_member_this_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_destructor_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() != 1) return false;
    std::string_view name_view{fn.name};
    std::string_view delete_suffix{"_delete"};
    if (name_view.size() < delete_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - delete_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == delete_suffix)) return false;
    return is_special_member_this_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_default_constructor_function(const Function& fn) {
    return is_constructor_function(fn) && fn.params.size() == 1;
}

[[nodiscard]] inline bool is_copy_constructor_function(const Function& fn) {
    return is_constructor_function(fn) && fn.params.size() == 2 &&
           is_special_member_const_lvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_move_constructor_function(const Function& fn) {
    return is_constructor_function(fn) && fn.params.size() == 2 &&
           is_special_member_rvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_copy_assignment_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() != 2) return false;
    std::string_view name_view{fn.name};
    std::string_view assign_suffix{"_operator_assign"};
    if (name_view.size() < assign_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - assign_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == assign_suffix)) return false;
    return is_special_member_this_param(fn.params[0].type, fn.member_owner_class) &&
           is_special_member_const_lvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_move_assignment_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() != 2) return false;
    std::string_view name_view{fn.name};
    std::string_view assign_suffix{"_operator_assign"};
    if (name_view.size() < assign_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - assign_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == assign_suffix)) return false;
    return is_special_member_this_param(fn.params[0].type, fn.member_owner_class) &&
           is_special_member_rvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_defaulted_special_member_equivalent_to_implicit_omission(const Function& fn) {
    return fn.is_defaulted &&
           (is_default_constructor_function(fn) || is_copy_constructor_function(fn) || is_move_constructor_function(fn) ||
            is_copy_assignment_function(fn) || is_move_assignment_function(fn));
}

[[nodiscard]] inline bool is_equality_operator_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() != 2) return false;
    std::string_view name_view{fn.name};
    std::string_view equal_suffix{"_operator_equal"};
    if (name_view.size() < equal_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - equal_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == equal_suffix)) return false;
    return is_member_receiver_self_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_inequality_operator_function(const Function& fn) {
    if (fn.member_owner_class.size() == 0 || fn.params.size() != 2) return false;
    std::string_view name_view{fn.name};
    std::string_view not_equal_suffix{"_operator_not_equal"};
    if (name_view.size() < not_equal_suffix.size()) return false;
    std::size_t suffix_pos = name_view.size() - not_equal_suffix.size();
    std::string_view actual_suffix{name_view.substr(suffix_pos)};
    if (!(actual_suffix == not_equal_suffix)) return false;
    return is_member_receiver_self_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_equality_like_operator_function(const Function& fn) {
    return is_equality_operator_function(fn) || is_inequality_operator_function(fn);
}

[[nodiscard]] inline bool is_defaulted_equality_operator_function(const Function& fn) {
    return fn.is_defaulted && is_equality_like_operator_function(fn);
}

[[nodiscard]] inline std::string equality_operator_method_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return std::string{"operator_equal"};
        case BinaryOp::Ne: return std::string{"operator_not_equal"};
        default: return std::string{};
    }
}

[[nodiscard]] inline ExprPtr make_overloaded_equality_call_expr(const Expr& lhs, const Expr& rhs, BinaryOp op,
                                                                SourceLocation loc) {
    ExprPtr call = std::make_unique<Expr>();
    call->kind = ExprKind::Call;
    call->loc = std::move(loc);
    call->name = equality_operator_method_name(op);
    call->lhs = deep_clone_expr_with_loc(lhs, call->loc);
    call->args.push_back(deep_clone_expr_with_loc(rhs, call->loc));
    return call;
}

class StructField {
  public:
    virtual ~StructField() = default;
    StructField() = default;
    StructField(const StructField&) = default;
    StructField& operator=(const StructField&) = default;
    StructField(StructField&&) = default;
    StructField& operator=(StructField&&) = default;

    SourceLocation loc;
    Type type;
    std::string name;
    std::optional<Initializer> default_initializer;
    AccessSpecifier access = AccessSpecifier::Public;
    std::vector<AlignmentSpecifier> alignment_specs;
    std::uint64_t resolved_alignment = 0;
};

class StructDef {
  public:
    virtual ~StructDef() = default;
    StructDef() = default;
    StructDef(const StructDef&) = default;
    StructDef& operator=(const StructDef&) = default;
    StructDef(StructDef&&) = default;
    StructDef& operator=(StructDef&&) = default;

    SourceLocation loc;
    std::string name;
    std::vector<StructField> fields;
    // False for an ordinary `struct`, true for a `union`. Both reuse this
    // one AST node because they share the same "named aggregate with fields"
    // surface at the parser/type-reference level; later passes consult this
    // flag for layout and safety-rule differences (e.g. all union members
    // overlap at offset 0, and union-member access is unsafe-gated).
    bool is_union = false;
    // `[[scpp::packed]]` on a struct/union declaration -- requests C-style
    // packed layout (no implicit padding between fields, overall alignment 1)
    // for FFI-facing aggregates.
    bool is_packed = false;
    std::vector<AlignmentSpecifier> alignment_specs;
    std::uint64_t resolved_alignment = 0;
    // See Function::namespace_path/is_exported/owning_module above --
    // same meaning, applied to a struct declaration (ch11 §11.3's
    // "struct definitions" are part of v0.1's exportable surface).
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    // True only for a non-exported definition recovered from a compiled
    // module's structured compile-time payload. Such a declaration remains
    // invisible to ordinary import surface rendering, but still needs to be
    // carried into an importer so exported generic/constexpr bodies can
    // reference their reachable private helpers/types.
    bool is_compile_time_dependency = false;
    std::string owning_module;
    // ch05 §5.14: non-empty for a generic struct's own *template*
    // definition (`template<Concept T> struct Name { ... };`) -- its
    // single GenericTypeParam is always concept-constrained (never
    // bare, see GenericTypeParam's own comment). Never emitted to
    // codegen directly (see Codegen::generate, mirroring
    // Function::is_generic_template's identical exclusion) -- each
    // concrete instantiation (`Name<SomeType>`) instead gets a separate
    // monomorphized struct injected into Program::structs by the
    // Monomorphizer, with its own distinct mangled name.
    std::vector<GenericTypeParam> template_params;
    // True for an ordinary bodyless forward declaration with no field list
    // yet (e.g. `struct Node;`). Such a declaration introduces the name so
    // later source may form pointers/references to it before a matching full
    // definition appears, but contributes no layout on its own.
    bool is_forward_declaration = false;
    // Non-empty only for this template definition's own identity while it
    // remains symbolic (generic). Lets later compiler passes distinguish
    // multiple template definitions sharing the same exposed name. Empty on
    // an ordinary concrete struct.
    std::string template_owner_id;
    // ch05 §5.15: `[[scpp::thread_movable]]`/`[[scpp::thread_shareable]]`
    // attached directly to this struct's own declaration (after the
    // `struct` keyword, before its name) -- manually asserts the
    // corresponding property holds for this type, unconditionally
    // overriding what the structural derivation (Monomorphizer's own
    // thread_movable_of/thread_shareable_of) would otherwise conclude
    // on its own (mirrors Rust's `unsafe impl Send`/`unsafe impl Sync`).
    // The attribute's mere presence always asserts the property *true*
    // -- there is no way to assert one *false* (real C++ has no stable
    // "negative impl" syntax to reuse either, matching this document's
    // own erasure-driven scoping). false (the default) means "no
    // override; use the structural derivation instead", not "asserted
    // false".
    bool thread_movable_override = false;
    bool thread_shareable_override = false;
    bool is_nodiscard = false;
    std::string nodiscard_reason;
};

class ClassField {
  public:
    virtual ~ClassField() = default;
    ClassField() = default;
    ClassField(const ClassField&) = default;
    ClassField& operator=(const ClassField&) = default;
    ClassField(ClassField&&) = default;
    ClassField& operator=(ClassField&&) = default;

    SourceLocation loc;
    Type type;
    std::string name;
    std::optional<Initializer> default_initializer;
    // ch04 §4.2: a member variable can never be Public -- rejected right
    // where it's parsed (parse_class_def), not deferred to a later pass
    // -- but the specifier is still recorded here (rather than simply
    // never representing it) so this stays a plain, uniform data shape,
    // matching StructField's own style.
    AccessSpecifier access = AccessSpecifier::Private;
    std::vector<AlignmentSpecifier> alignment_specs;
    std::uint64_t resolved_alignment = 0;
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
class ClassDef {
  public:
    virtual ~ClassDef() = default;
    ClassDef() = default;
    ClassDef(const ClassDef& other);
    ClassDef& operator=(const ClassDef& other);
    ClassDef(ClassDef&&) = default;
    ClassDef& operator=(ClassDef&&) = default;

    SourceLocation loc;
    std::string name;
    std::vector<ClassField> fields;
    // See Function::namespace_path/is_exported/owning_module above --
    // same meaning. `is_exported` on the ClassDef itself (set by
    // `export class Name { ... };`) also propagates to every method
    // synthesized from this class into Program::functions, so exporting
    // a class is one declaration, not one per member (ch11 §11.3).
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    bool is_compile_time_dependency = false;
    std::string owning_module;
    // ch05 §5.11: true for a hidden class synthesized from a `concept`
    // declaration's own requirement list (one bodyless method per
    // requirement) -- never a real, user-written class. Exists purely so
    // a generic function's own body-check can resolve method calls on
    // its concept-constrained parameter through the exact same class/
    // method-call machinery used everywhere else (Param::type's innermost
    // Named type names this synthesized class while checking the
    // template's own definition), with zero new movecheck logic. Excluded
    // entirely from codegen (see Codegen::generate) -- it (and its
    // bodyless methods) never needs to exist as real emitted code, since
    // every call site is monomorphized against a real concrete type
    // instead (see Function::is_generic_template).
    bool is_concept_witness = false;
    // ch05 §5.14: non-empty for a generic class's own *template*
    // definition (`template<typename T> class Name { ... };`, or
    // `template<Concept T> class Name { ... };`) -- its single
    // GenericTypeParam may be bare (concept_name empty) or constrained;
    // see GenericTypeParam's own comment. Every method may additionally
    // layer its own `requires Concept<T>` clause (Function::
    // method_requires_concept), decomposing the "what does T support"
    // question per member rather than needing one shared, class-wide
    // constraint. Never emitted to codegen directly (mirrors
    // is_concept_witness/Function::is_generic_template's identical
    // exclusion) -- each concrete instantiation (`Name<SomeType>`)
    // instead gets a separate monomorphized class injected into
    // Program::classes by the Monomorphizer.
    std::vector<GenericTypeParam> template_params;
    // Non-empty only for this template definition's own identity while it
    // remains symbolic (generic). Methods parsed from the class body store
    // the same id in Function::generic_method_owner_id so movecheck can
    // recover their exact owning template definition even when multiple
    // primary/specialized templates share this one exposed class name.
    // Empty on every ordinary concrete class.
    std::string template_owner_id;
    // True for an ordinary class template forward declaration with no body
    // (e.g. `template<typename Sig> class function;`). Such a declaration
    // introduces the name and its primary template parameter list but is
    // never itself directly instantiated unless some later definition or
    // partial specialization supplies a body to match.
    bool is_forward_declaration = false;
    // ch05 §5.14: true only for a *temporary, internal* witness-
    // substituted class synthesized purely to check one generic method's
    // body once, abstractly, at its own definition (mirrors a concept's
    // own witness class, is_concept_witness, but distinct from it: this
    // exists per generic-*type*-method-check, never user-facing, and
    // deliberately not reused/cached across methods -- see the
    // Monomorphizer's own comment). Excluded from codegen exactly like
    // is_concept_witness.
    bool is_synthetic_check_only = false;
    // `[[scpp::interface]]` on this class definition.
    bool is_interface = false;
    std::vector<AlignmentSpecifier> alignment_specs;
    std::uint64_t resolved_alignment = 0;
    // Direct base-specifiers in source order. Existing behavior still only
    // *uses* one ordinary base operationally, but the AST/model now stores
    // the generalized shape needed for later interface/multiple-
    // inheritance phases.
    std::vector<BaseSpecifier> base_specifiers;
    // Class-scope `using Base::member;` declarations in source order.
    std::vector<ClassUsingDeclaration> using_declarations;
    // ch05 §5.14: true for a variadic generic type's own *primary
    // template* declaration -- `template<typename... Ts> class Tuple;`
    // (a bodyless forward declaration; `fields`/`base_specifiers` are
    // always empty/default for one of these). Exists purely to
    // register the name (so `Tuple<...>` parses as a type, and so a
    // later specialization of it can be recognized/validated) --
    // itself never instantiated directly; see is_variadic_specialization.
    bool is_variadic_primary_template = false;
    // ch05 §5.14: true for one of the exactly two fixed patterns
    // specializing an already-declared variadic primary template --
    // `template<> class Tuple<> { ... };` (the empty-pack base case,
    // template_params empty) or `template<typename Head, typename...
    // Tail> class Tuple<Head, Tail...> { ... };` (the recursive case,
    // template_params == [Head, Tail(is_pack)]) -- no other shape is
    // legal (parser-enforced), matching the doc's own "exactly two
    // fixed patterns, not general/arbitrary specialization" scoping.
    // Multiple ClassDefs may share the same `name` this way (one per
    // specialization) -- ordinary lookups that need "the" definition of
    // a generic type (e.g. an ordinary, non-variadic instantiation)
    // never see more than one, since is_variadic_primary_template/
    // is_variadic_specialization are mutually exclusive with an
    // ordinary generic type's own single ClassDef.
    bool is_variadic_specialization = false;
    // True for an ordinary (non-variadic) partial specialization pattern,
    // e.g. `template<typename R, typename... Args> class
    // function<R(Args...)> { ... };`. `specialization_template_args` then
    // holds the symbolic `<...>` pattern matched against a concrete
    // instantiation's original template arguments.
    bool is_partial_specialization = false;
    std::vector<Type> specialization_template_args;
    // ch05 §5.15: see StructDef::thread_movable_override's own comment
    // -- identical meaning, applied to a class declaration instead
    // (after the `class` keyword, before its name).
    bool thread_movable_override = false;
    bool thread_shareable_override = false;
    // ch05 §5.15: `[[scpp::thread_movable_if(a, b)]]` on a class
    // declaration -- a parameterized override of the class's own
    // thread_movable/thread_shareable values, evaluated per concrete
    // instantiation. Null means "no conditional override; fall back to the
    // unconditional booleans above or, if those are both false, the
    // structural derivation".
    ExprPtr thread_movable_if_movable_expr;
    ExprPtr thread_movable_if_shareable_expr;
    bool is_nodiscard = false;
    std::string nodiscard_reason;

    [[nodiscard]] std::optional<std::reference_wrapper<const BaseSpecifier>> direct_ordinary_base() const {
        for (std::size_t i = 0; i < base_specifiers.size(); i++) {
            const BaseSpecifier& base = base_specifiers[i];
            if (base.kind != BaseClassKind::Interface) {
                return std::optional<std::reference_wrapper<const BaseSpecifier>>{
                    std::reference_wrapper<const BaseSpecifier>{base}};
            }
        }
        return std::optional<std::reference_wrapper<const BaseSpecifier>>{};
    }
};

inline Function::Function(const Function& other)
    : return_type{other.return_type},
      name{other.name},
      loc{other.loc},
      params{other.params},
      return_lifetime{other.return_lifetime},
      body{},
      is_extern_c{other.is_extern_c},
      is_module_extern{other.is_module_extern},
      is_unsafe{other.is_unsafe},
      is_nodiscard{other.is_nodiscard},
      nodiscard_reason{other.nodiscard_reason},
      is_compile_time_dependency{other.is_compile_time_dependency},
      skip_imported_body_verification{other.skip_imported_body_verification},
      eval_mode{other.eval_mode},
      has_varargs{other.has_varargs},
      method_requires_concept{other.method_requires_concept},
      is_generic_template{other.is_generic_template},
      template_params{other.template_params},
      generic_method_owner_id{other.generic_method_owner_id},
      member_owner_class{other.member_owner_class},
      access_context_class{other.access_context_class},
      member_initializers{other.member_initializers},
      receiver_ref_qualifier{other.receiver_ref_qualifier},
      is_static{other.is_static},
      access{other.access},
      is_virtual{other.is_virtual},
      is_override{other.is_override},
      is_pure{other.is_pure},
      is_defaulted{other.is_defaulted},
      expects_out_of_line_definition{other.expects_out_of_line_definition},
      forwards_to{other.forwards_to},
      namespace_path{other.namespace_path},
      is_exported{other.is_exported},
      owning_module{other.owning_module},
      visibility_module{other.visibility_module},
      superseded_forward_declaration_locs{other.superseded_forward_declaration_locs} {
    if (other.body != nullptr) this->body = deep_clone_stmt(*other.body);
}

inline Function& Function::operator=(const Function& other) {
    Function clone{other};
    *this = std::move(clone);
    return *this;
}

inline ClassDef::ClassDef(const ClassDef& other)
    : loc{other.loc},
      name{other.name},
      fields{other.fields},
      namespace_path{other.namespace_path},
      is_exported{other.is_exported},
      is_compile_time_dependency{other.is_compile_time_dependency},
      owning_module{other.owning_module},
      is_concept_witness{other.is_concept_witness},
      template_params{other.template_params},
      template_owner_id{other.template_owner_id},
      is_forward_declaration{other.is_forward_declaration},
      is_synthetic_check_only{other.is_synthetic_check_only},
      is_interface{other.is_interface},
      alignment_specs{other.alignment_specs},
      resolved_alignment{other.resolved_alignment},
      base_specifiers{other.base_specifiers},
      using_declarations{other.using_declarations},
      is_variadic_primary_template{other.is_variadic_primary_template},
      is_variadic_specialization{other.is_variadic_specialization},
      is_partial_specialization{other.is_partial_specialization},
      specialization_template_args{other.specialization_template_args},
      thread_movable_override{other.thread_movable_override},
      thread_shareable_override{other.thread_shareable_override},
      thread_movable_if_movable_expr{},
      thread_movable_if_shareable_expr{},
      is_nodiscard{other.is_nodiscard},
      nodiscard_reason{other.nodiscard_reason} {
    if (other.thread_movable_if_movable_expr != nullptr) {
        this->thread_movable_if_movable_expr = deep_clone_expr(*other.thread_movable_if_movable_expr);
    }
    if (other.thread_movable_if_shareable_expr != nullptr) {
        this->thread_movable_if_shareable_expr = deep_clone_expr(*other.thread_movable_if_shareable_expr);
    }
}

inline ClassDef& ClassDef::operator=(const ClassDef& other) {
    ClassDef clone{other};
    *this = std::move(clone);
    return *this;
}

// ch05 §5.11: one requirement inside a `concept Name = requires(...) {
// ... };` body -- restricted (a pragmatic v0.1 scoping cut, matching the
// spec's own examples) to a method call on the requires-expression's own
// placeholder parameter: `{ placeholder.method(args) };` (simple -- see
// has_return_constraint below) or `{ placeholder.method(args) } ->
// std::same_as<T>;` (compound, exact-type only, never
// std::convertible_to -- ch05 §5.11's own reasoning: scpp has no
// implicit scalar conversions at all, so the two would mean the same
// thing anyway). Spec §13.2 additionally permits a compound requirement
// (never a simple one -- §13.2(3) scopes this to "an unqualified
// compound-requirement of the form { E }" only) whose own `{ E }` has no
// trailing `-> constraint`, to instead be a construction expression --
// see is_construct/construct_type_name below. Arbitrary *other*
// expressions (e.g. a binary operator), type-requirements
// (`typename T::Foo;`), and nested requirements (arbitrary boolean
// constant-expressions) are explicitly out of scope for v0.1.
class ConceptRequirement {
  public:
    virtual ~ConceptRequirement() = default;
    ConceptRequirement() = default;
    ConceptRequirement(const ConceptRequirement&) = default;
    ConceptRequirement& operator=(const ConceptRequirement&) = default;

    // True for a construction-shaped compound requirement (spec
    // §13.2(3.3)): `{ T(args...) };` or `{ T{args...} };`. When true,
    // method_name is unused (there is no method being called at all --
    // arg_types/arg_lifetimes below instead describe the constructor
    // call's own argument list) and construct_type_name/has_return_
    // constraint take over describing the requirement's shape --
    // construction shape can never carry a trailing `-> std::same_as<T>`
    // constraint (spec §13.2(3)'s own precondition), so has_return_
    // constraint is always false whenever this is true.
    bool is_construct = false;
    // Only meaningful when is_construct is true: the spelled name of the
    // type being constructed, e.g. "T" in `requires(T t) { T{t}; }` --
    // almost always the concept's own template_param_name (ConceptDef),
    // substituted for the concrete type under test at concept-
    // satisfaction time (see generics_support.cppm's
    // type_satisfies_concept), but spec §13.2(3.3) permits any named
    // type, so a fixed, already-declared type name is also accepted and
    // left un-substituted.
    std::string construct_type_name;
    std::string method_name;
    // The call's own argument types (e.g. `f(x)` where `x: int` ->
    // {int}) -- excludes the implicit receiver (the placeholder itself),
    // exactly like Function::params excludes nothing but `this` is
    // always params[0] elsewhere; here there is no receiver slot at all
    // since the placeholder is never itself part of this list. When
    // is_construct is true, this instead holds the construction
    // expression's own argument types (e.g. `T{t}` -> {T}, with "T"
    // represented as a Named type spelled exactly like the concept's own
    // template_param_name, substituted for the concrete type under test
    // at concept-satisfaction time) -- the placeholder itself may appear
    // here (unlike the call-shaped forms above), since a construction
    // expression's whole point is normally to probe the placeholder's
    // own type (e.g. copy-constructibility, `T{t}`).
    std::vector<Type> arg_types;
    // Parallel to arg_types: the corresponding probe parameter's own
    // `[[scpp::lifetime(...)]]` annotation (spec §6.2(13.1)), or a
    // default-constructed (absent) LifetimeAnnotation when that probe
    // parameter bears none. Per spec §6.2(22)-(22.4), this constrains
    // concept satisfaction itself -- see generics_support.cppm's own
    // type_satisfies_concept, which compares this declaration-local
    // grouping relation (same-spelling => same group, different-spelling
    // => different group, `any` => must also be `any`) against
    // each candidate declaration's own corresponding parameters. Always
    // the same length as arg_types.
    std::vector<LifetimeAnnotation> arg_lifetimes;
    // True for a compound requirement (`{ expr } -> std::same_as<T>;`).
    // False (the common case) for a simple requirement (`{ expr };`),
    // which constrains nothing about the result's type -- ch05 §5.11:
    // the generic body may then only use the call as a discarded
    // expression-statement, never bind its result to anything.
    bool has_return_constraint = false;
    Type return_type; // only meaningful when has_return_constraint
};

// ch05 §5.11: `template<typename T> concept Name = requires(<param>) {
// <requirements> };` -- concepts are always declared with the full
// `template<typename T>` header (unlike a *function*, which only ever
// uses the abbreviated `Concept auto` form in v0.1: real C++ grammar has
// no other way to spell a concept declaration itself, so this header is
// unavoidable here even though ch05 §5.11 otherwise avoids introducing
// the general `template<...>` machinery).
class ConceptDef {
  public:
    virtual ~ConceptDef() = default;
    ConceptDef() = default;
    ConceptDef(const ConceptDef&) = default;
    ConceptDef& operator=(const ConceptDef&) = default;
    ConceptDef(ConceptDef&&) = default;
    ConceptDef& operator=(ConceptDef&&) = default;
    std::string name;
    // The template header's own type-parameter name, e.g. "T" in
    // `template<typename T>` -- recorded so the parser can recognize
    // later uses of this exact identifier inside the requires-expression
    // as referring to the constrained type (e.g. `const T& t`), rather
    // than an ordinary (already-declared) type name.
    std::string template_param_name;
    // The requires-expression's own placeholder parameter name, e.g.
    // "t" in `requires(const T& t) { ... }` -- every requirement's
    // method calls are written against this name.
    std::string requires_param_name;
    // True exactly when the requires-expression declared that placeholder
    // as `const` (e.g. `requires(const T& t) { ... }`) rather than a
    // mutable placeholder (`requires(T t) { ... }`). Used when deciding
    // whether a concrete candidate method really satisfies the concept's
    // own requirement on a const receiver.
    bool requires_param_is_const = false;
    std::vector<ConceptRequirement> requirements;
    // See Function::namespace_path/is_exported/owning_module above --
    // same meaning, applied to a concept declaration (ch11 §11.3's
    // exportable surface).
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    std::string owning_module;
};

class EnumVariant {
  public:
    virtual ~EnumVariant() = default;
    EnumVariant() = default;
    EnumVariant(const EnumVariant&) = default;
    EnumVariant& operator=(const EnumVariant&) = default;

    std::string name;
    std::int64_t value = 0;
};

class EnumDef {
  public:
    virtual ~EnumDef() = default;
    EnumDef()
        : name{}, underlying_type{}, variants{}, namespace_path{}, owning_module{} {
        underlying_type = named_type("int");
    }
    EnumDef(const EnumDef&) = default;
    EnumDef& operator=(const EnumDef&) = default;
    EnumDef(EnumDef&&) = default;
    EnumDef& operator=(EnumDef&&) = default;

    std::string name;
    Type underlying_type;
    std::vector<EnumVariant> variants;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    bool is_compile_time_dependency = false;
    std::string owning_module;
};

// ch11 §11.8: one `import name;` / `export import name;` declaration,
// or (ch11 §11.4) a same-module partition import (`import :part;` /
// `export import :part;`).
class ImportDecl {
  public:
    virtual ~ImportDecl() = default;
    ImportDecl() = default;
    ImportDecl(const ImportDecl&) = default;
    ImportDecl& operator=(const ImportDecl&) = default;

    // The imported module's dotted name (e.g. "std", "org.lotx.cmath"),
    // exactly as written -- this is also the key the driver's
    // ModuleResolver/import-path mapping (`--import name=path`) is
    // looked up by. For a partition import (is_partition == true), this
    // instead holds just the bare partition identifier (e.g. "string"),
    // no dots -- the parser resolves it against the *current* file's own
    // module_name (joined as "<module_name>:<this>") before consulting
    // the resolver, so the resolver callback's key shape is identical
    // either way.
    std::string module_name;
    // ch11 §11.8: true for `export import name;` (transitively
    // re-exports `name`'s own exports to whoever imports *this* file in
    // turn), false for a plain `import name;` (private, non-transitive).
    // For a partition import (ch11 §11.4), this instead controls whether
    // the partition's own exported declarations become part of the
    // *whole module's* export surface (export import :part;) or stay
    // purely internal to the module (plain import :part;) -- either way
    // every declaration in the partition, exported or not, is visible to
    // the current file and its sibling partitions (see
    // parser.cppm's merge_partition).
    bool is_reexport = false;
    // ch11 §11.4: true for `import :part;` / `export import :part;` (a
    // same-module partition import) -- false for an ordinary cross-
    // module `import name;` (ch11 §11.8). A partition import is resolved
    // and merged completely differently from a cross-module one (see
    // parser.cppm's merge_partition vs merge_imported_module): every
    // declaration crosses in (not just exported ones), with bodies
    // preserved (the partition compiles *together* with the importing
    // file, not as a separately-compiled module).
    bool is_partition = false;
};

class Program {
  public:
    virtual ~Program() = default;
    Program() = default;
    Program(const Program&) = default;
    Program& operator=(const Program&) = default;

    std::vector<StructDef> structs;
    std::vector<ClassDef> classes;
    std::vector<EnumDef> enums;
    std::vector<TypeAliasDecl> type_aliases;
    std::vector<Function> functions;
    std::vector<GlobalVar> globals;
    // ch05 §5.11: every `concept` declaration parsed from this file (or
    // merged in from an imported module -- concepts participate in
    // export/import exactly like a struct/class declaration).
    std::vector<ConceptDef> concepts;
    // Absolute path of the source file this Program was parsed from when
    // one is known (e.g. a real CLI/driver build from disk); empty for
    // in-memory/unit-test sources that have no backing file path.
    std::string source_path;

    // ch11 §11.3: this file's own module name, e.g. "std" or
    // "org.lotx.cmath" -- empty for an ordinary, non-module file (every
    // scpp file before this chapter, and still the overwhelmingly common
    // case: nothing about module_name being empty changes any existing
    // behavior anywhere). For a partition file (ch11 §11.4, `export
    // module std:string;`), this still holds just the base module name
    // ("std", never "std:string") -- see partition_name below for the
    // part after the colon.
    std::string module_name;
    // ch11 §11.4: the partition name after `:` in `export module
    // name:part;` / `module name:part;` -- empty for the primary
    // interface/implementation unit (every module file before this
    // section, and still the common case). A non-empty partition_name
    // designates exactly one file within module_name; see parser.cppm's
    // merge_partition for how a partition's declarations reach the file
    // that imports it.
    std::string partition_name;
    // True for a file starting `export module name;` or `export module
    // name:part;` (an interface unit or interface partition -- may
    // contain `export`-marked declarations).
    bool is_module_interface = false;
    // True for a file starting `module name;` or `module name:part;`
    // with no `export` (an implementation unit or implementation
    // partition -- contributes more code to the same module, but may
    // not itself export anything; see ch11 §11.3/§11.4). Mutually
    // exclusive with is_module_interface.
    bool is_module_impl = false;
    // Every `import`/`export import` declaration this file has
    // (cross-module or same-module partition alike), in source order --
    // consulted by the driver to know which modules must be separately
    // compiled and linked in, and by the export/namespace validation
    // pass for re-export bookkeeping.
    std::vector<ImportDecl> imports;
};

using OptionalProgramRef = std::optional<std::reference_wrapper<const Program>>;

[[nodiscard]] inline const GlobalVar*
find_visible_global(std::optional<std::reference_wrapper<const Program [[scpp::lifetime(program)]]>> program,
                    const std::vector<std::string>& namespace_path,
                    const std::string& name, bool explicit_global_qualification = false) [[scpp::lifetime(program)]] {
    if (!program.has_value()) return nullptr;
    auto matches_name = [&](const GlobalVar& global, std::string_view candidate) {
        if (global.decl.get() == nullptr) return false;
        std::string_view global_name{global.decl->var_name};
        return global_name == candidate;
    };
    if (explicit_global_qualification) {
        for (std::size_t i = 0; i < program->get().globals.size(); i++) {
            const GlobalVar& global = program->get().globals[i];
            if (matches_name(global, name)) return &global;
        }
        return nullptr;
    }
    for (std::size_t depth = namespace_path.size(); depth > 0; depth--) {
        std::string candidate{};
        for (std::size_t i = 0; i < depth; i++) {
            if (candidate.size() != 0) candidate += "::";
            candidate += namespace_path[i];
        }
        candidate += "::";
        candidate += name;
        for (std::size_t i = 0; i < program->get().globals.size(); i++) {
            const GlobalVar& global = program->get().globals[i];
            if (matches_name(global, candidate)) return &global;
        }
    }
    for (std::size_t i = 0; i < program->get().globals.size(); i++) {
        const GlobalVar& global = program->get().globals[i];
        if (matches_name(global, name)) return &global;
    }
    return nullptr;
}

// ch06 §6 fixes scpp's scalar types at exactly twenty distinct names,
// and every phase asks the same small set of questions about them: is
// this name a scalar at all, is it integral or floating or bool, is it
// signed, how wide is it, which values fit in it.
//
// Those questions used to be answered independently in roughly twenty
// places -- three separate copies of the twenty-name set (two of them in
// the same file), six of "is this integral", six of "is this floating",
// two each of signedness, width and bounds -- spread across the parser,
// movecheck, constant evaluation and codegen. They agreed only by
// coincidence and repeated repair: `char`'s signedness was answered
// "signed" by six sites and "unsigned" by two until very recently, and
// the width answers still disagree about `size_t` on any target whose
// pointers are not 64 bits.
//
// `scalar_type_info` below is now the only place in the compiler that
// lists the twenty names. Everything else derives from it: the
// predicates immediately following, `named_scalar_layout`, movecheck's
// literal-range check, constant evaluation's bounds, codegen's LLVM type
// mapping and its DWARF encoding. Adding a scalar type, or changing one's
// signedness or width, is a single edit here and cannot leave some other
// phase behind.
//
// This lives in `scpp.ast` because that is the one module every other
// one imports (movecheck, constexpression and codegen all do, directly
// or through their primary interface), and because a lower layer would
// have to depend on LLVM -- which the mapping deliberately does not.
enum class ScalarCategory { Bool, Integral, Floating };

class ScalarTypeInfo {
public:
    ScalarCategory category = ScalarCategory::Integral;

    // Width in bits. For `size_t`/`ptrdiff_t` this is the width on a
    // 64-bit target; `is_pointer_sized` marks that the real width is
    // whatever the target's pointers are, and callers that know the
    // target (see `named_scalar_layout` and codegen's `to_llvm_type`)
    // substitute it. Callers that do not -- movecheck's literal-range
    // check -- get the host's pointer width, matching TargetLayoutInfo's
    // own default.
    int bit_width = 0;

    // `bool` is marked unsigned even though it is not an integral
    // scalar: it is an i8 holding exactly 0 or 1 (ch06's false=0/true=1
    // invariant), so widening it must zero-extend or `true` would read
    // as -1. The two questions that consult this field ask it over
    // different domains -- `is_unsigned_scalar_type_name` asks only of
    // integral scalars, because it drives `icmp`/`sdiv`/negation, where
    // `bool` never appears; the cast path asks of every scalar, because
    // every scalar can be widened.
    bool is_unsigned = false;

    bool is_pointer_sized = false;

    virtual ~ScalarTypeInfo() = default;
    ScalarTypeInfo() = default;
    ScalarTypeInfo(ScalarCategory category, int bit_width, bool is_unsigned, bool is_pointer_sized)
        : category{category}, bit_width{bit_width}, is_unsigned{is_unsigned}, is_pointer_sized{is_pointer_sized} {
        return;
    }
    ScalarTypeInfo(const ScalarTypeInfo&) = default;
    ScalarTypeInfo(ScalarTypeInfo&&) = default;
};

// The twenty names of ch06 §6, grouped by width so the table can be
// audited at a glance. Note that `int` is not an alias for `int32_t`,
// nor `long` for `int64_t`, nor `float` for `float32_t`: they are
// distinct types of equal width, which is exactly why the entries repeat
// rather than collapse.
[[nodiscard]] inline std::optional<ScalarTypeInfo> scalar_type_info(std::string_view name) {
    if (name == "bool") {
        ScalarTypeInfo info{ScalarCategory::Bool, 8, true, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "char" || name == "int8_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 8, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "uint8_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 8, true, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "int16_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 16, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "uint16_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 16, true, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "int" || name == "int32_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 32, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "unsigned int" || name == "uint32_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 32, true, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "long" || name == "int64_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 64, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "unsigned long" || name == "uint64_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 64, true, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    // `long`/`unsigned long` above are always 64-bit regardless of target
    // (ch06's deliberate anti-LP64/LLP64-pitfall fix); only these two
    // track the pointer width.
    if (name == "ptrdiff_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 64, false, true};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "size_t") {
        ScalarTypeInfo info{ScalarCategory::Integral, 64, true, true};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "float" || name == "float32_t") {
        ScalarTypeInfo info{ScalarCategory::Floating, 32, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    if (name == "double" || name == "float64_t") {
        ScalarTypeInfo info{ScalarCategory::Floating, 64, false, false};
        return std::optional<ScalarTypeInfo>{info};
    }
    return std::optional<ScalarTypeInfo>{};
}

// `TypeKind::Named` alone cannot tell a scalar apart from a struct,
// class or witness name -- all four share that TypeKind -- so this
// checks the closed set ch06 documents rather than the type's own kind.
[[nodiscard]] inline bool is_scalar_type_name(std::string_view name) { return scalar_type_info(name).has_value(); }

[[nodiscard]] inline bool is_integral_scalar_type_name(std::string_view name) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return false;
    const ScalarTypeInfo& value = *info;
    return value.category == ScalarCategory::Integral;
}

[[nodiscard]] inline bool is_float_scalar_type_name(std::string_view name) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return false;
    const ScalarTypeInfo& value = *info;
    return value.category == ScalarCategory::Floating;
}

// Asked only of integral scalars, because the instructions it selects --
// `icmp slt` vs `ult`, `sdiv` vs `udiv`, negation -- are integral-only.
// See ScalarTypeInfo::is_unsigned for why `bool` answers false here but
// still zero-extends when widened.
[[nodiscard]] inline bool is_unsigned_scalar_type_name(std::string_view name) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return false;
    const ScalarTypeInfo& value = *info;
    return value.category == ScalarCategory::Integral && value.is_unsigned;
}

// Does widening this scalar zero-extend? Unlike the predicate above this
// is asked of every scalar, so `bool` answers true: it is an i8 holding
// 0 or 1 and must widen to 1, not -1.
[[nodiscard]] inline bool scalar_widens_unsigned(std::string_view name) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return false;
    const ScalarTypeInfo& value = *info;
    return value.is_unsigned;
}

struct TargetLayoutInfo {
    std::uint64_t pointer_size_bytes = sizeof(void*);
    std::uint64_t pointer_align_bytes = alignof(void*);
};

// The width of a scalar in bits, given the target's pointer width. Only
// `size_t`/`ptrdiff_t` consult `pointer_bit_width`; everything else has
// a fixed width. Returns 0 for a name that is not a scalar.
[[nodiscard]] inline int scalar_bit_width(std::string_view name, int pointer_bit_width) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return 0;
    const ScalarTypeInfo& value = *info;
    if (value.is_pointer_sized && pointer_bit_width > 0) return pointer_bit_width;
    return value.bit_width;
}

[[nodiscard]] inline int host_pointer_bit_width() { return static_cast<int>(sizeof(void*) * 8); }

// 2**exponent, for 0 <= exponent <= 62. Written as a doubling loop
// rather than `1 << exponent` because this file is self-hosted -- it is
// compiled by scpp as well as by clang++ (see src/scpp.toml), and scpp
// has no shift operator. Every caller below bounds `exponent` at 63
// before calling, so the result always fits an std::int64_t.
[[nodiscard]] inline std::int64_t two_to_the(int exponent) {
    std::int64_t result = 1;
    for (int i = 0; i < exponent; i = i + 1) {
        result = result * 2;
    }
    return result;
}

// INT64_MAX/INT64_MIN as functions rather than named constants, and
// INT64_MIN built by negation rather than spelled out, because this file
// is self-hosted: scpp gives an untyped integer literal the type of the
// place it initializes, so a bare literal in a return statement adopts
// std::int64_t, but `-9223372036854775807 - 1` is an expression between
// two literals and is evaluated as `int`.
[[nodiscard]] inline std::int64_t scalar_int64_max() { return 9223372036854775807; }

[[nodiscard]] inline std::int64_t scalar_int64_min() {
    std::int64_t max_value = 9223372036854775807;
    return -max_value - 1;
}

// The inclusive range of values a scalar can hold, as std::int64_t.
//
// A 64-bit unsigned type cannot state its true upper bound here, and
// neither of its two consumers can represent one: movecheck carries
// literal values as std::int64_t and so does constant evaluation, so
// anything above INT64_MAX has already wrapped by the time it arrives
// and cannot be told apart from a genuinely negative value. The two
// resolve that the same way -- clamp the maximum to INT64_MAX -- and
// differ only in what they do with a negative, which is each one's own
// policy rather than a property of the type; see
// `integer_literal_value_fits` below.
class ScalarValueRange {
public:
    std::int64_t min_value = 0;
    std::int64_t max_value = 0;

    virtual ~ScalarValueRange() = default;
    ScalarValueRange() = default;
    ScalarValueRange(std::int64_t min_value, std::int64_t max_value) : min_value{min_value}, max_value{max_value} {
        return;
    }
    ScalarValueRange(const ScalarValueRange&) = default;
    ScalarValueRange(ScalarValueRange&&) = default;
};

[[nodiscard]] inline std::optional<ScalarValueRange> scalar_value_range(std::string_view name, int pointer_bit_width) {
    std::optional<ScalarTypeInfo> info = scalar_type_info(name);
    if (!info.has_value()) return std::optional<ScalarValueRange>{};
    const ScalarTypeInfo& scalar = *info;
    if (scalar.category == ScalarCategory::Bool) {
        ScalarValueRange range{0, 1};
        return std::optional<ScalarValueRange>{range};
    }
    if (scalar.category == ScalarCategory::Floating) return std::optional<ScalarValueRange>{};
    int bits = scalar_bit_width(name, pointer_bit_width);
    if (scalar.is_unsigned) {
        std::int64_t max_value = bits >= 64 ? scalar_int64_max() : two_to_the(bits) - 1;
        ScalarValueRange range{0, max_value};
        return std::optional<ScalarValueRange>{range};
    }
    if (bits >= 64) {
        ScalarValueRange range{scalar_int64_min(), scalar_int64_max()};
        return std::optional<ScalarValueRange>{range};
    }
    ScalarValueRange range{-two_to_the(bits - 1), two_to_the(bits - 1) - 1};
    return std::optional<ScalarValueRange>{range};
}

// ch06 §6: does the untyped integer literal `value` name a value of
// `type_name`? A literal has no type of its own -- it adopts the type of
// the place it initializes -- but that only works when the value it
// spells is actually one of that type's values. `int8_t x = 300;` does
// not spell an int8_t, and `unsigned int x = 4294967296;` does not spell
// an unsigned int, so treating them as compatible would smuggle in
// exactly the silent, lossy conversion the scalar-conversion rule exists
// to forbid.
//
// A 64-bit unsigned target is deliberately unconstrained on the low end:
// a literal above INT64_MAX has already wrapped to a negative by the
// time it arrives (see `scalar_value_range`), so rejecting negatives
// would reject the legitimate spelling. The narrower unsigned types have
// no such ambiguity and are checked normally.
[[nodiscard]] inline bool integer_literal_value_fits(std::int64_t value, std::string_view type_name, int pointer_bit_width) {
    std::optional<ScalarValueRange> range = scalar_value_range(type_name, pointer_bit_width);
    if (!range.has_value()) return true;
    const ScalarValueRange& bounds = *range;
    if (value < 0 && is_unsigned_scalar_type_name(type_name)) {
        return scalar_bit_width(type_name, pointer_bit_width) >= 64;
    }
    return value >= bounds.min_value && value <= bounds.max_value;
}

// ch06 §6: which type does a literal take from the place it appears in?
//
// A literal has no type of its own, so it adopts the type of the place
// that consumes it -- but only when the value it spells is one of that
// type's values, and only for a place whose type that kind of literal
// can name at all. These four predicates answer that, and they are here
// rather than in move checking because two layers ask the question and
// they used to answer it differently: move checking implemented the
// rule, while constant evaluation gave every integer literal the type
// `int` and every floating literal `double`. The layers therefore
// disagreed about which programs are valid -- `constexpr int8_t v = 5;`
// passed move checking and was then rejected by the constexpr evaluator
// as an int-to-int8_t assignment, so a program's validity depended on
// which layer looked at it. One question, one answer, one place.
//
// A place spelled `T&` is the place `T`; `int8_t& r = ...; r = 5;` is
// the same adoption question as the unreferenced form.
[[nodiscard]] inline const Type& literal_adoption_target(const Type& type) {
    if (type.kind == TypeKind::Reference && type.pointee != nullptr) return *type.pointee;
    return type;
}

// `bool` and `char` are excluded deliberately: they are scalars, but
// neither is nameable by an integer literal. `bool b = 1;` and
// `char c = 65;` are conversions, not spellings -- `true`/`false` and
// `'A'` are how those values are written.
[[nodiscard]] inline bool integer_literal_may_adopt_type(const Type& type) {
    if (type.kind != TypeKind::Named) return false;
    if (type.name == "bool" || type.name == "char") return false;
    std::string_view spelled_name{type.name};
    return is_scalar_type_name(spelled_name);
}

// The parser leaves `-128` as a unary minus applied to `128`, so a
// consumer matching only on ExprKind::IntegerLiteral would see a Unary
// here and conclude the expression carries a type of its own -- making
// `int8_t x = -128;` a conversion from `int`, which it is not: -128
// spells an int8_t value directly. Unwrapping one level of negation
// keeps every consumer agreeing on that.
[[nodiscard]] inline bool is_untyped_numeric_literal(const Expr& expr) {
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Neg && expr.lhs != nullptr) {
        return expr.lhs->kind == ExprKind::IntegerLiteral || expr.lhs->kind == ExprKind::FloatLiteral;
    }
    return expr.kind == ExprKind::IntegerLiteral || expr.kind == ExprKind::FloatLiteral;
}

[[nodiscard]] inline bool literal_adopts_type(const Expr& literal, const Type& type, int pointer_bit_width) {
    const Type& target = literal_adoption_target(type);
    if (literal.kind == ExprKind::Unary && literal.unary_op == UnaryOp::Neg && literal.lhs != nullptr) {
        if (literal.lhs->kind == ExprKind::FloatLiteral) {
            if (target.kind != TypeKind::Named) return false;
            std::string_view float_name{target.name};
            return is_float_scalar_type_name(float_name);
        }
        if (literal.lhs->kind == ExprKind::IntegerLiteral) {
            if (!integer_literal_may_adopt_type(target)) return false;
            std::string_view negated_name{target.name};
            return integer_literal_value_fits(-literal.lhs->int_value, negated_name, pointer_bit_width);
        }
        return false;
    }
    if (literal.kind == ExprKind::IntegerLiteral) {
        if (!integer_literal_may_adopt_type(target)) return false;
        std::string_view integer_name{target.name};
        return integer_literal_value_fits(literal.int_value, integer_name, pointer_bit_width);
    }
    if (literal.kind == ExprKind::FloatLiteral) {
        if (target.kind != TypeKind::Named) return false;
        std::string_view float_name{target.name};
        return is_float_scalar_type_name(float_name);
    }
    if (literal.kind == ExprKind::BoolLiteral) return target.kind == TypeKind::Named && target.name == "bool";
    if (literal.kind == ExprKind::CharLiteral) return target.kind == TypeKind::Named && target.name == "char";
    // ch06 §6: `nullptr` compares against any raw pointer type
    // (`p == nullptr`), and against `nullptr_t` itself
    // (`nullptr == nullptr`, and a `nullptr_t`-typed place compared with
    // the literal). It is compatible with nothing else -- in particular
    // with no integer type, so `i == nullptr` stays the error it has
    // always been.
    if (literal.kind == ExprKind::NullptrLiteral) {
        return target.kind == TypeKind::Pointer || target.kind == TypeKind::FunctionPointer || is_nullptr_type(target);
    }
    return false;
}

class TypeLayoutInfo {
public:
    std::uint64_t size_bytes = 0;
    std::uint64_t abi_align_bytes = 1;

    virtual ~TypeLayoutInfo() = default;
    TypeLayoutInfo() = default;
    TypeLayoutInfo(std::uint64_t size_bytes, std::uint64_t abi_align_bytes)
        : size_bytes{size_bytes}, abi_align_bytes{abi_align_bytes} {
        return;
    }
    TypeLayoutInfo(const TypeLayoutInfo&) = default;
    TypeLayoutInfo(TypeLayoutInfo&&) = default;
};

[[nodiscard]] inline TypeLayoutInfo value_of_type_layout(const std::optional<TypeLayoutInfo>& layout) {
    const TypeLayoutInfo& layout_ref = *layout;
    std::uint64_t size_bytes = layout_ref.size_bytes;
    std::uint64_t abi_align_bytes = layout_ref.abi_align_bytes;
    TypeLayoutInfo value{};
    value.size_bytes = size_bytes;
    value.abi_align_bytes = abi_align_bytes;
    return value;
}

[[nodiscard]] inline std::optional<TypeLayoutInfo> layout_of_type(const Program& program, const Type& type,
                                                                  TargetLayoutInfo target = {}) {
    class LayoutComputer {
      public:
        virtual ~LayoutComputer() = default;
        LayoutComputer(const Program& program, TargetLayoutInfo target) : program{program}, target{target} {}
        const Program& program;
        TargetLayoutInfo target;
        std::unordered_set<std::string> visiting_named_types{};

        [[nodiscard]] static std::uint64_t align_up(std::uint64_t value, std::uint64_t align) {
            if (align <= 1) return value;
            return ((value + align - 1) / align) * align;
        }

        [[nodiscard]] std::optional<std::reference_wrapper<const EnumDef>> find_enum(std::string_view name) const {
            for (std::size_t i = 0; i < program.enums.size(); i++) {
                const EnumDef& def = program.enums[i];
                std::string_view def_name{def.name};
                if (def_name == name) {
                    return std::optional<std::reference_wrapper<const EnumDef>>{std::reference_wrapper<const EnumDef>{def}};
                }
            }
            return std::optional<std::reference_wrapper<const EnumDef>>{};
        }

        [[nodiscard]] std::optional<std::reference_wrapper<const StructDef>> find_struct(std::string_view name) const {
            std::optional<std::reference_wrapper<const StructDef>> forward_decl{};
            for (std::size_t i = 0; i < program.structs.size(); i++) {
                const StructDef& def = program.structs[i];
                std::string_view def_name{def.name};
                if (def_name != name) continue;
                if (!def.is_forward_declaration) {
                    return std::optional<std::reference_wrapper<const StructDef>>{
                        std::reference_wrapper<const StructDef>{def}};
                }
                if (!forward_decl.has_value()) {
                    forward_decl = std::optional<std::reference_wrapper<const StructDef>>{
                        std::reference_wrapper<const StructDef>{def}};
                }
            }
            return forward_decl;
        }

        [[nodiscard]] std::optional<std::reference_wrapper<const ClassDef>> find_class(std::string_view name) const {
            std::optional<std::reference_wrapper<const ClassDef>> forward_decl{};
            for (std::size_t i = 0; i < program.classes.size(); i++) {
                const ClassDef& def = program.classes[i];
                std::string_view def_name{def.name};
                if (def_name != name) continue;
                if (!def.is_forward_declaration) {
                    return std::optional<std::reference_wrapper<const ClassDef>>{
                        std::reference_wrapper<const ClassDef>{def}};
                }
                if (!forward_decl.has_value()) {
                    forward_decl = std::optional<std::reference_wrapper<const ClassDef>>{
                        std::reference_wrapper<const ClassDef>{def}};
                }
            }
            return forward_decl;
        }

        // Derived entirely from `scalar_type_info` -- see its comment for
        // why this file is the one place the twenty names are listed.
        // Size follows the width; alignment equals size for every scalar
        // scpp has, except that `size_t`/`ptrdiff_t` take the target's
        // pointer alignment, which need not equal its pointer size.
        [[nodiscard]] std::optional<TypeLayoutInfo> named_scalar_layout(std::string_view name) const {
            std::optional<ScalarTypeInfo> info = scalar_type_info(name);
            if (!info.has_value()) return std::optional<TypeLayoutInfo>{};
            const ScalarTypeInfo& scalar = *info;
            if (scalar.is_pointer_sized) {
                std::uint64_t align = target.pointer_align_bytes < 1 ? static_cast<std::uint64_t>(1) : target.pointer_align_bytes;
                TypeLayoutInfo layout{target.pointer_size_bytes, align};
                return std::optional<TypeLayoutInfo>{layout};
            }
            std::uint64_t size_bytes = static_cast<std::uint64_t>(scalar.bit_width) / 8;
            TypeLayoutInfo layout{size_bytes, size_bytes};
            return std::optional<TypeLayoutInfo>{layout};
        }

        [[nodiscard]] std::optional<TypeLayoutInfo> compute(const Type& current) {
            if (current.kind == TypeKind::Pointer || current.kind == TypeKind::Reference ||
                current.kind == TypeKind::FunctionPointer) {
                std::uint64_t align = target.pointer_align_bytes < 1 ? static_cast<std::uint64_t>(1) : target.pointer_align_bytes;
                if ((current.kind == TypeKind::Pointer || current.kind == TypeKind::Reference) &&
                    current.pointee != nullptr && current.pointee->kind == TypeKind::Named) {
                    std::optional<std::reference_wrapper<const ClassDef>> referent{find_class(current.pointee->name)};
                    if (referent.has_value() && referent->get().is_interface) {
                        TypeLayoutInfo layout{target.pointer_size_bytes * 2, align};
                        return std::optional<TypeLayoutInfo>{layout};
                    }
                }
                TypeLayoutInfo layout{target.pointer_size_bytes, align};
                return std::optional<TypeLayoutInfo>{layout};
            }
            if (current.kind == TypeKind::Function) return std::optional<TypeLayoutInfo>{};
            if (current.kind == TypeKind::Span) {
                std::uint64_t pointer_align = target.pointer_align_bytes < 1 ? static_cast<std::uint64_t>(1) : target.pointer_align_bytes;
                std::uint64_t count_align = 8;
                std::uint64_t size = align_up(target.pointer_size_bytes, count_align) + 8;
                TypeLayoutInfo layout{align_up(size, std::max(pointer_align, count_align)),
                                      std::max(pointer_align, count_align)};
                return std::optional<TypeLayoutInfo>{layout};
            }
            if (current.kind == TypeKind::Array) {
                if (current.element == nullptr || current.array_size < 0) return std::optional<TypeLayoutInfo>{};
                std::optional<TypeLayoutInfo> element = this->compute(*current.element);
                if (!element.has_value()) return std::optional<TypeLayoutInfo>{};
                TypeLayoutInfo layout{element->size_bytes * static_cast<std::uint64_t>(current.array_size),
                                      element->abi_align_bytes};
                return std::optional<TypeLayoutInfo>{layout};
            }
            if (current.kind == TypeKind::Named) {
                if (current.name == "void") return std::optional<TypeLayoutInfo>{};
                std::optional<TypeLayoutInfo> scalar{named_scalar_layout(current.name)};
                if (scalar.has_value()) return scalar;
                std::optional<std::reference_wrapper<const EnumDef>> enum_def{find_enum(current.name)};
                if (enum_def.has_value()) return this->compute(enum_def->get().underlying_type);
                if (visiting_named_types.contains(current.name)) return std::optional<TypeLayoutInfo>{};
                visiting_named_types.insert(current.name);
                std::optional<std::reference_wrapper<const StructDef>> struct_def{find_struct(current.name)};
                if (struct_def.has_value()) {
                    const StructDef& struct_ref = struct_def->get();
                    if (struct_ref.is_forward_declaration) {
                        this->visiting_named_types.erase(current.name);
                        return std::optional<TypeLayoutInfo>{};
                    }
                    if (!struct_ref.is_union) {
                        std::uint64_t offset = 0;
                        std::uint64_t overall_align = 1;
                        for (std::size_t i = 0; i < struct_ref.fields.size(); i++) {
                            const StructField& field = struct_ref.fields[i];
                            std::optional<TypeLayoutInfo> field_layout = this->compute(field.type);
                            if (!field_layout.has_value()) {
                                this->visiting_named_types.erase(current.name);
                                return std::optional<TypeLayoutInfo>{};
                            }
                            TypeLayoutInfo field_layout_value{value_of_type_layout(field_layout)};
                            std::uint64_t field_align =
                                struct_ref.is_packed ? 1 : std::max(field_layout_value.abi_align_bytes, field.resolved_alignment);
                            offset = align_up(offset, field_align);
                            offset += field_layout_value.size_bytes;
                            overall_align = std::max(overall_align, field_align);
                        }
                        overall_align =
                            struct_ref.is_packed ? 1 : std::max(overall_align, struct_ref.resolved_alignment);
                        this->visiting_named_types.erase(current.name);
                        TypeLayoutInfo layout{align_up(offset, overall_align), overall_align};
                        return std::optional<TypeLayoutInfo>{layout};
                    }
                    if (struct_ref.fields.size() == 0) {
                        this->visiting_named_types.erase(current.name);
                        return std::optional<TypeLayoutInfo>{};
                    }
                    std::uint64_t max_size = 0;
                    std::uint64_t overall_align = 1;
                    for (std::size_t i = 0; i < struct_ref.fields.size(); i++) {
                        const StructField& field = struct_ref.fields[i];
                        std::optional<TypeLayoutInfo> field_layout = this->compute(field.type);
                        if (!field_layout.has_value()) {
                            this->visiting_named_types.erase(current.name);
                            return std::optional<TypeLayoutInfo>{};
                        }
                        TypeLayoutInfo field_layout_value{value_of_type_layout(field_layout)};
                        max_size = std::max(max_size, field_layout_value.size_bytes);
                        std::uint64_t field_align =
                            struct_ref.is_packed ? 1 : std::max(field_layout_value.abi_align_bytes, field.resolved_alignment);
                        overall_align = std::max(overall_align, field_align);
                    }
                    overall_align = struct_ref.is_packed ? 1 : std::max(overall_align, struct_ref.resolved_alignment);
                    this->visiting_named_types.erase(current.name);
                    TypeLayoutInfo layout{align_up(max_size, overall_align), overall_align};
                    return std::optional<TypeLayoutInfo>{layout};
                }
                std::optional<std::reference_wrapper<const ClassDef>> class_def{find_class(current.name)};
                if (class_def.has_value()) {
                    const ClassDef& class_ref = class_def->get();
                    if (class_ref.is_forward_declaration) {
                        this->visiting_named_types.erase(current.name);
                        return std::optional<TypeLayoutInfo>{};
                    }
                    std::uint64_t offset = 0;
                    std::uint64_t overall_align = 1;
                    std::optional<std::reference_wrapper<const BaseSpecifier>> base{class_ref.direct_ordinary_base()};
                    if (base.has_value()) {
                        std::optional<TypeLayoutInfo> base_layout = this->compute(base->get().base_type);
                        if (!base_layout.has_value()) {
                            this->visiting_named_types.erase(current.name);
                            return std::optional<TypeLayoutInfo>{};
                        }
                        TypeLayoutInfo base_layout_value{value_of_type_layout(base_layout)};
                        offset = base_layout_value.size_bytes;
                        overall_align = std::max(overall_align, base_layout_value.abi_align_bytes);
                    }
                    if (!class_ref.is_interface && offset == 0) {
                        offset = target.pointer_size_bytes;
                        overall_align = std::max(overall_align, target.pointer_align_bytes);
                    }
                    for (std::size_t i = 0; i < class_ref.fields.size(); i++) {
                        const ClassField& field = class_ref.fields[i];
                        std::optional<TypeLayoutInfo> field_layout = this->compute(field.type);
                        if (!field_layout.has_value()) {
                            this->visiting_named_types.erase(current.name);
                            return std::optional<TypeLayoutInfo>{};
                        }
                        TypeLayoutInfo field_layout_value{value_of_type_layout(field_layout)};
                        std::uint64_t field_align = std::max(field_layout_value.abi_align_bytes, field.resolved_alignment);
                        offset = align_up(offset, field_align);
                        offset += field_layout_value.size_bytes;
                        overall_align = std::max(overall_align, field_align);
                    }
                    overall_align = std::max(overall_align, class_ref.resolved_alignment);
                    this->visiting_named_types.erase(current.name);
                    TypeLayoutInfo layout{align_up(offset, overall_align), overall_align};
                    return std::optional<TypeLayoutInfo>{layout};
                }
                this->visiting_named_types.erase(current.name);
                return std::optional<TypeLayoutInfo>{};
            }
            return std::optional<TypeLayoutInfo>{};
        }
    };

    LayoutComputer layout{program, target};
    return layout.compute(type);
}

// The one place a `Type` is rendered for a *diagnostic* -- as the user
// would recognize it, not as codegen mangles it. Lives here, beside the
// `Type` it prints, so movecheck and codegen render the same type the
// same way: a call rejected by one and described by the other must not
// name the same type two different ways, and a second copy of this
// switch in another module would drift the moment a `TypeKind` is added
// (`layout_of_type` above is the other switch over `TypeKind` in this
// file; both must be extended together).
//
// Split into one helper per kind, like parser.cppm's type_to_string, so
// that every case of the dispatcher below ends in a bare `return ...;`:
// this file is itself compiled by scpp under the self-hosting probe, and
// scpp's switch-case grammar (validate_switch_fallthrough) requires that.
[[nodiscard]] inline std::string describe_type_brief(const Type& type);

[[nodiscard]] inline std::string describe_type_brief_named(const Type& type) {
    std::string result{""};
    if (type.is_const_qualified) result += "const ";
    result += type.name;
    if (!type.template_args.empty()) {
        result += "<";
        for (std::size_t i = 0; i < type.template_args.size(); i++) {
            if (i != 0) result += ",";
            result += describe_type_brief(type.template_args[i]);
        }
        result += ">";
    }
    return result;
}

[[nodiscard]] inline std::string describe_type_brief_reference(const Type& type) {
    if (type.pointee == nullptr) return "&?";
    std::string result{""};
    result += describe_type_brief(*type.pointee);
    if (type.is_rvalue_ref) {
        result += "&&";
    } else if (type.is_mutable_ref) {
        result += "&mut";
    } else {
        result += "&";
    }
    return result;
}

[[nodiscard]] inline std::string describe_type_brief_pointer(const Type& type) {
    if (type.pointee == nullptr) return "*?";
    std::string result{""};
    result += describe_type_brief(*type.pointee);
    result += "*";
    return result;
}

[[nodiscard]] inline std::string describe_type_brief_array(const Type& type) {
    if (type.element == nullptr) return "?[]";
    std::string result{""};
    result += describe_type_brief(*type.element);
    result += "[";
    result += std::to_string(type.array_size);
    result += "]";
    return result;
}

[[nodiscard]] inline std::string describe_type_brief_span(const Type& type) {
    if (type.element == nullptr) return "std::span<?>";
    std::string result{""};
    result += "std::span<";
    result += describe_type_brief(*type.element);
    result += ">";
    return result;
}

[[nodiscard]] inline std::string describe_type_brief_function(const Type& type) {
    std::string result{""};
    if (type.function_return == nullptr) {
        result += "void";
    } else {
        result += describe_type_brief(*type.function_return);
    }
    if (type.kind == TypeKind::FunctionPointer) {
        result += " (*)(";
    } else {
        result += " (";
    }
    for (std::size_t i = 0; i < type.function_params.size(); i++) {
        if (i != 0) result += ", ";
        result += describe_type_brief(type.function_params[i]);
    }
    result += ")";
    return result;
}

[[nodiscard]] inline std::string describe_type_brief(const Type& type) {
    switch (type.kind) {
        case TypeKind::Named: return describe_type_brief_named(type);
        case TypeKind::Reference: return describe_type_brief_reference(type);
        case TypeKind::Pointer: return describe_type_brief_pointer(type);
        case TypeKind::Array: return describe_type_brief_array(type);
        case TypeKind::Span: return describe_type_brief_span(type);
        case TypeKind::Function: return describe_type_brief_function(type);
        case TypeKind::FunctionPointer: return describe_type_brief_function(type);
    }
    return "<type>";
}

} // namespace scpp
