module;

export module scpp.ast;

import std;

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
    std::shared_ptr<const std::string> source_path;

    [[nodiscard]] bool is_known() const { return line > 0; }
    [[nodiscard]] bool has_source_path() const { return source_path != nullptr && !source_path->empty(); }
    [[nodiscard]] const std::string& source_path_text() const {
        static const std::string empty;
        return source_path ? *source_path : empty;
    }

};

[[nodiscard]] inline SourceLocation make_source_location(int line, int column,
                                                        std::shared_ptr<const std::string> source_path = {}) {
    return SourceLocation{line, column, std::move(source_path)};
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

// A type reference. `pointee`/`element` use shared_ptr (not unique_ptr) so
// Type stays copyable: Param/StructField/Stmt store Type by value, and
// copying a pointer/array type is just a cheap refcount bump, not a deep
// clone.
struct Type {
    TypeKind kind = TypeKind::Named;

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
    // (constexpr.cppm's AlignmentResolver, extended for ch05 §9.4)
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

    // Reference only: true for `T&&` (ch03's "passed by move" parameter
    // form -- ownership transfer, exactly like a std::unique_ptr move,
    // just for any type). This is genuinely just a named rvalue
    // reference: unlike real C++'s `auto&&`/forwarding-reference
    // template parameters, scpp's `auto` never triggers reference
    // collapsing, so `T&&` always means "take ownership via move," never
    // "bind to either an lvalue or rvalue depending on the argument" --
    // confirmed against the language-definition doc, ch05 §5.11's
    // `Concept auto&&` generic form reuses this exact same flag (see
    // Param::generic_concept). false for `T&`/`const T&`, where
    // is_mutable_ref (above) then distinguishes those two as before.
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

struct AlignmentSpecifier {
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

struct LifetimeAnnotation {
    // Empty when no `[[scpp::lifetime(...)]]` is present on this
    // declaration. Otherwise the raw identifier spelled in source --
    // either the reserved word `any` or a user-written, declaration-
    // local group name.
    std::string name;

    [[nodiscard]] bool present() const { return !name.empty(); }
    [[nodiscard]] bool is_any() const { return name == "any"; }

    bool operator==(const LifetimeAnnotation&) const = default;
};

[[nodiscard]] inline Type named_type(std::string name) {
    Type type{};
    type.kind = TypeKind::Named;
    type.name = std::move(name);
    return type;
}

struct Param {
    Type type;
    std::string name;
    LifetimeAnnotation lifetime;
    std::shared_ptr<Expr> default_expr;
    // ch05 §5.11: empty for an ordinary parameter (the overwhelmingly
    // common case). Non-empty names the concept this parameter is
    // constrained by, for the abbreviated generic-function form --
    // `ConceptName auto name` (by value), `ConceptName auto& name`/
    // `const ConceptName auto& name` (mutable/shared borrow), or
    // `ConceptName auto&& name` (move-in) -- mirroring the ordinary
    // `T`/`T&`/`const T&`/`T&&` forms exactly, just with `auto`
    // interposed and a concept name standing in for a concrete type
    // (see parse_generic_param_type). `type` itself is still fully
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
struct LambdaCapture {
    std::string name;
    bool by_reference = false;
    // Non-null only for an init-capture; null for a plain `[name]`/
    // `[&name]` capture (whose value/binding comes directly from the
    // enclosing scope's own same-named local).
    ExprPtr init;
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
struct ExplicitTemplateArg {
    bool is_type = true;
    Type type;                 // meaningful when is_type
    std::shared_ptr<Expr> value; // meaningful when !is_type
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
    std::string name;

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
    ExprPtr lhs;
    ExprPtr rhs;
    ExprPtr third;
    bool fold_ellipsis_on_left = false;

    // Unary (operand stored in `lhs`)
    UnaryOp unary_op{};

    // Call arguments / New constructor arguments
    std::vector<ExprPtr> args;

    // ch05 §5.11/§5.14: Call only -- non-empty only for a call to a
    // full-header-form generic function that explicitly supplies one or
    // more of its own template arguments at the call site (e.g.
    // `make<Circle>()`, `get<2>(t)`) -- see ExplicitTemplateArg's own
    // comment. Empty for every other call (the overwhelmingly common
    // case: an ordinary, non-generic call, or a generic call resolved
    // entirely by argument-position deduction).
    std::vector<ExplicitTemplateArg> explicit_template_args;

    // New: the allocated element type `T` in `new T...`.
    // Destroy: the explicitly-named destroyed type `T` in `expr.~T()` /
    // `ptr->~T()`.
    // Lambda: the explicit trailing return type (`-> Type`), only
    // meaningful when has_lambda_explicit_return_type is true.
    // Cast: the target type `T` in `static_cast<T>(expr)`/`(T)expr`
    // (operand stored in `lhs`, like Unary).
    // Sizeof(type): the queried type operand `T`.
    Type type;
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
    std::vector<LambdaCapture> lambda_captures;
    LambdaCaptureMode lambda_blanket_mode = LambdaCaptureMode::None;
    // The lambda's own parameter list -- ordinary Params; a concept-
    // constrained lambda parameter is not supported in this version
    // (mirrors parser.cppm's reject_generic_params for methods).
    std::vector<Param> lambda_params;
    bool has_lambda_explicit_return_type = false;
    // `[x](int y) mutable { ... }` -- licenses the synthesized "call"
    // method to modify by-value-captured fields (a non-`const` method,
    // mirroring an ordinary method's own trailing `const`, ch05 §5.9).
    bool lambda_is_mutable = false;
    StmtPtr lambda_body;
};

enum class StmtKind {
    VarDecl,
    Return,
    If,
    While,
    Break,
    Continue,
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

struct Stmt {
    StmtKind kind;

    // Where this statement begins in the source file -- same purpose as
    // Expr::loc above.
    SourceLocation loc;

    // VarDecl
    Type type;
    std::string var_name;
    ExprPtr init; // optional
    std::vector<AlignmentSpecifier> alignment_specs;
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
    std::vector<ExprPtr> ctor_args;

    // Return / ExprStmt (value/expr)
    ExprPtr expr;

    // If / While
    ExprPtr condition;
    IfMode if_mode = IfMode::Runtime;
    StmtPtr then_branch;
    StmtPtr else_branch; // optional, If only

    // Block
    std::vector<StmtPtr> statements;
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

inline void rewrite_expr_locs(Expr& expr, const SourceLocation& loc) {
    expr.loc = loc;
    if (expr.lhs) rewrite_expr_locs(*expr.lhs, loc);
    if (expr.rhs) rewrite_expr_locs(*expr.rhs, loc);
    if (expr.third) rewrite_expr_locs(*expr.third, loc);
    for (ExprPtr& arg : expr.args) rewrite_expr_locs(*arg, loc);
    for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
        if (!arg.is_type && arg.value) rewrite_expr_locs(*arg.value, loc);
    }
    for (LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init) rewrite_expr_locs(*capture.init, loc);
    }
    for (Param& param : expr.lambda_params) {
        if (param.default_expr) rewrite_expr_locs(*param.default_expr, loc);
    }
    if (expr.lambda_body) {
        auto rewrite_stmt_locs = [&](auto&& self, Stmt& stmt) -> void {
            stmt.loc = loc;
            if (stmt.init) rewrite_expr_locs(*stmt.init, loc);
            for (ExprPtr& arg : stmt.ctor_args) rewrite_expr_locs(*arg, loc);
            if (stmt.expr) rewrite_expr_locs(*stmt.expr, loc);
            if (stmt.condition) rewrite_expr_locs(*stmt.condition, loc);
            if (stmt.then_branch) self(self, *stmt.then_branch);
            if (stmt.else_branch) self(self, *stmt.else_branch);
            for (StmtPtr& child : stmt.statements) self(self, *child);
        };
        rewrite_stmt_locs(rewrite_stmt_locs, *expr.lambda_body);
    }
}

[[nodiscard]] inline Param deep_clone_param(const Param& param) {
    Param clone = param;
    if (param.default_expr) clone.default_expr = std::shared_ptr<Expr>(deep_clone_expr(*param.default_expr).release());
    return clone;
}

[[nodiscard]] inline ExprPtr deep_clone_expr(const Expr& expr) {
    auto clone = std::make_unique<Expr>();
    clone->kind = expr.kind;
    clone->loc = expr.loc;
    clone->int_value = expr.int_value;
    clone->float_value = expr.float_value;
    clone->bool_value = expr.bool_value;
    clone->name = expr.name;
    clone->explicit_global_qualification = expr.explicit_global_qualification;
    clone->binary_op = expr.binary_op;
    clone->unary_op = expr.unary_op;
    clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
    clone->sizeof_operand_is_type = expr.sizeof_operand_is_type;
    clone->type = expr.type;
    clone->has_paren_init = expr.has_paren_init;
    clone->destroy_through_pointer = expr.destroy_through_pointer;
    clone->through_arrow = expr.through_arrow;
    clone->implicit_arrow_deref = expr.implicit_arrow_deref;
    clone->implicit_arrow_chain_safe = expr.implicit_arrow_chain_safe;
    if (expr.lhs) clone->lhs = deep_clone_expr(*expr.lhs);
    if (expr.rhs) clone->rhs = deep_clone_expr(*expr.rhs);
    if (expr.third) clone->third = deep_clone_expr(*expr.third);
    for (const ExprPtr& arg : expr.args) clone->args.push_back(deep_clone_expr(*arg));
    for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
        ExplicitTemplateArg cloned_arg = arg;
        if (!arg.is_type && arg.value) cloned_arg.value = std::shared_ptr<Expr>(deep_clone_expr(*arg.value).release());
        clone->explicit_template_args.push_back(std::move(cloned_arg));
    }
    for (const LambdaCapture& capture : expr.lambda_captures) {
        LambdaCapture cloned_capture;
        cloned_capture.name = capture.name;
        cloned_capture.by_reference = capture.by_reference;
        if (capture.init) cloned_capture.init = deep_clone_expr(*capture.init);
        clone->lambda_captures.push_back(std::move(cloned_capture));
    }
    clone->lambda_blanket_mode = expr.lambda_blanket_mode;
    for (const Param& param : expr.lambda_params) clone->lambda_params.push_back(deep_clone_param(param));
    clone->has_lambda_explicit_return_type = expr.has_lambda_explicit_return_type;
    clone->lambda_is_mutable = expr.lambda_is_mutable;
    if (expr.lambda_body) clone->lambda_body = deep_clone_stmt(*expr.lambda_body);
    return clone;
}

[[nodiscard]] inline StmtPtr deep_clone_stmt(const Stmt& stmt) {
    auto clone = std::make_unique<Stmt>();
    clone->kind = stmt.kind;
    clone->loc = stmt.loc;
    clone->type = stmt.type;
    clone->var_name = stmt.var_name;
    if (stmt.init) clone->init = deep_clone_expr(*stmt.init);
    clone->alignment_specs = stmt.alignment_specs;
    clone->resolved_alignment = stmt.resolved_alignment;
    clone->is_const = stmt.is_const;
    clone->is_constexpr = stmt.is_constexpr;
    clone->is_static_local = stmt.is_static_local;
    clone->has_ctor_args = stmt.has_ctor_args;
    for (const ExprPtr& arg : stmt.ctor_args) clone->ctor_args.push_back(deep_clone_expr(*arg));
    if (stmt.expr) clone->expr = deep_clone_expr(*stmt.expr);
    if (stmt.condition) clone->condition = deep_clone_expr(*stmt.condition);
    clone->if_mode = stmt.if_mode;
    if (stmt.then_branch) clone->then_branch = deep_clone_stmt(*stmt.then_branch);
    if (stmt.else_branch) clone->else_branch = deep_clone_stmt(*stmt.else_branch);
    for (const StmtPtr& child : stmt.statements) clone->statements.push_back(deep_clone_stmt(*child));
    clone->is_unsafe = stmt.is_unsafe;
    return clone;
}

[[nodiscard]] inline ExprPtr deep_clone_expr_with_loc(const Expr& expr, const SourceLocation& loc) {
    ExprPtr clone = deep_clone_expr(expr);
    rewrite_expr_locs(*clone, loc);
    return clone;
}

struct GlobalVar {
    StmtPtr decl;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    std::string owning_module;
};

struct Initializer {
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

struct MemberInitializer {
    std::string member_name;
    Initializer initializer;
    SourceLocation loc;

    MemberInitializer() = default;
    MemberInitializer(const MemberInitializer&) = default;
    MemberInitializer& operator=(const MemberInitializer&) = default;
    MemberInitializer(MemberInitializer&&) = default;
    MemberInitializer& operator=(MemberInitializer&&) = default;
};

[[nodiscard]] inline StmtPtr clone_initializer_stmt(const Stmt& stmt);

[[nodiscard]] inline ExprPtr clone_initializer_expr(const Expr& expr) {
    auto clone = std::make_unique<Expr>();
    clone->kind = expr.kind;
    clone->loc = expr.loc;
    clone->int_value = expr.int_value;
    clone->float_value = expr.float_value;
    clone->bool_value = expr.bool_value;
    clone->name = expr.name;
    clone->explicit_global_qualification = expr.explicit_global_qualification;
    clone->binary_op = expr.binary_op;
    if (expr.lhs) clone->lhs = clone_initializer_expr(*expr.lhs);
    if (expr.rhs) clone->rhs = clone_initializer_expr(*expr.rhs);
    if (expr.third) clone->third = clone_initializer_expr(*expr.third);
    clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
    clone->unary_op = expr.unary_op;
    for (const ExprPtr& arg : expr.args) clone->args.push_back(clone_initializer_expr(*arg));
    clone->explicit_template_args = expr.explicit_template_args;
    for (ExplicitTemplateArg& arg : clone->explicit_template_args) {
        if (arg.value) arg.value = std::shared_ptr<Expr>(clone_initializer_expr(*arg.value).release());
    }
    clone->type = expr.type;
    clone->sizeof_operand_is_type = expr.sizeof_operand_is_type;
    clone->has_paren_init = expr.has_paren_init;
    clone->destroy_through_pointer = expr.destroy_through_pointer;
    clone->through_arrow = expr.through_arrow;
    clone->implicit_arrow_deref = expr.implicit_arrow_deref;
    clone->implicit_arrow_chain_safe = expr.implicit_arrow_chain_safe;
    clone->lambda_captures.clear();
    for (const LambdaCapture& capture : expr.lambda_captures) {
        LambdaCapture cloned;
        cloned.name = capture.name;
        cloned.by_reference = capture.by_reference;
        if (capture.init) cloned.init = clone_initializer_expr(*capture.init);
        clone->lambda_captures.push_back(std::move(cloned));
    }
    clone->lambda_blanket_mode = expr.lambda_blanket_mode;
    for (const Param& param : expr.lambda_params) clone->lambda_params.push_back(deep_clone_param(param));
    clone->has_lambda_explicit_return_type = expr.has_lambda_explicit_return_type;
    clone->lambda_is_mutable = expr.lambda_is_mutable;
    if (expr.lambda_body) clone->lambda_body = clone_initializer_stmt(*expr.lambda_body);
    return clone;
}

[[nodiscard]] inline StmtPtr clone_initializer_stmt(const Stmt& stmt) {
    auto clone = std::make_unique<Stmt>();
    clone->kind = stmt.kind;
    clone->loc = stmt.loc;
    clone->type = stmt.type;
    clone->var_name = stmt.var_name;
    if (stmt.init) clone->init = clone_initializer_expr(*stmt.init);
    clone->alignment_specs = stmt.alignment_specs;
    clone->resolved_alignment = stmt.resolved_alignment;
    clone->is_const = stmt.is_const;
    clone->is_constexpr = stmt.is_constexpr;
    clone->is_static_local = stmt.is_static_local;
    clone->has_ctor_args = stmt.has_ctor_args;
    for (const ExprPtr& arg : stmt.ctor_args) clone->ctor_args.push_back(clone_initializer_expr(*arg));
    if (stmt.expr) clone->expr = clone_initializer_expr(*stmt.expr);
    if (stmt.condition) clone->condition = clone_initializer_expr(*stmt.condition);
    clone->if_mode = stmt.if_mode;
    if (stmt.then_branch) clone->then_branch = clone_initializer_stmt(*stmt.then_branch);
    if (stmt.else_branch) clone->else_branch = clone_initializer_stmt(*stmt.else_branch);
    for (const StmtPtr& nested : stmt.statements) clone->statements.push_back(clone_initializer_stmt(*nested));
    clone->is_unsafe = stmt.is_unsafe;
    return clone;
}

[[nodiscard]] inline GlobalVar clone_global_var(const GlobalVar& global) {
    GlobalVar clone;
    if (global.decl) clone.decl = clone_initializer_stmt(*global.decl);
    clone.namespace_path = global.namespace_path;
    clone.is_exported = global.is_exported;
    clone.owning_module = global.owning_module;
    return clone;
}

inline AlignmentSpecifier::AlignmentSpecifier(const AlignmentSpecifier& other)
    : loc(other.loc), operand_is_type(other.operand_is_type), type(other.type) {
    if (other.expr) expr = clone_initializer_expr(*other.expr);
}

inline AlignmentSpecifier& AlignmentSpecifier::operator=(const AlignmentSpecifier& other) {
    if (this == &other) return *this;
    AlignmentSpecifier clone(other);
    *this = std::move(clone);
    return *this;
}

inline Initializer::Initializer(const Initializer& other) : has_brace_args(other.has_brace_args) {
    if (other.expr) expr = clone_initializer_expr(*other.expr);
    for (const ExprPtr& arg : other.brace_args) brace_args.push_back(clone_initializer_expr(*arg));
}

inline Initializer& Initializer::operator=(const Initializer& other) {
    if (this == &other) return *this;
    Initializer clone(other);
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
struct GenericTypeParam {
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

struct BaseSpecifier {
    Type base_type;
    AccessSpecifier access = AccessSpecifier::Private;
    bool is_virtual = false;
    BaseClassKind kind = BaseClassKind::Unknown;
    // ch05 §5.14: meaningful for a variadic specialization's recursive
    // base-clause shape (e.g. the trailing `Tail...` in
    // `Tuple<Tail...>`). Empty for every other base-specifier.
    std::string pack_arg_name;
};

struct ClassUsingDeclaration {
    std::string base_name;
    std::string member_name;
    AccessSpecifier access = AccessSpecifier::Private;
};

struct TypeAliasDecl {
    SourceLocation loc;
    Type underlying_type;
    std::string name;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    std::string owning_module;
};

struct Function {
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
};

[[nodiscard]] inline bool special_member_owner_name_matches(std::string_view spelled_name, std::string_view owner_name) {
    if (spelled_name == owner_name) return true;
    std::size_t scope = owner_name.rfind("::");
    return scope != std::string_view::npos && spelled_name == owner_name.substr(scope + 2);
}

[[nodiscard]] inline bool is_special_member_this_param(const Type& type, std::string_view owner_name) {
    return type.kind == TypeKind::Reference && type.is_mutable_ref && type.pointee &&
           type.pointee->kind == TypeKind::Named && special_member_owner_name_matches(type.pointee->name, owner_name);
}

[[nodiscard]] inline bool is_special_member_const_lvalue_self_param(const Type& type, std::string_view owner_name) {
    return type.kind == TypeKind::Reference && !type.is_rvalue_ref && !type.is_mutable_ref && type.pointee &&
           type.pointee->kind == TypeKind::Named && special_member_owner_name_matches(type.pointee->name, owner_name);
}

[[nodiscard]] inline bool is_special_member_rvalue_self_param(const Type& type, std::string_view owner_name) {
    return type.kind == TypeKind::Reference && type.is_rvalue_ref && type.pointee &&
           type.pointee->kind == TypeKind::Named && special_member_owner_name_matches(type.pointee->name, owner_name);
}

[[nodiscard]] inline bool is_member_receiver_self_param(const Type& type, std::string_view owner_name) {
    return type.kind == TypeKind::Reference && !type.is_rvalue_ref && type.pointee &&
           type.pointee->kind == TypeKind::Named && special_member_owner_name_matches(type.pointee->name, owner_name);
}

[[nodiscard]] inline bool is_constructor_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_new") && !fn.params.empty() &&
           is_special_member_this_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_destructor_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_delete") && fn.params.size() == 1 &&
           is_special_member_this_param(fn.params[0].type, fn.member_owner_class);
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
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_assign") && fn.params.size() == 2 &&
           is_special_member_this_param(fn.params[0].type, fn.member_owner_class) &&
           is_special_member_const_lvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_move_assignment_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_assign") && fn.params.size() == 2 &&
           is_special_member_this_param(fn.params[0].type, fn.member_owner_class) &&
           is_special_member_rvalue_self_param(fn.params[1].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_defaulted_special_member_equivalent_to_implicit_omission(const Function& fn) {
    return fn.is_defaulted &&
           (is_default_constructor_function(fn) || is_copy_constructor_function(fn) || is_move_constructor_function(fn) ||
            is_copy_assignment_function(fn) || is_move_assignment_function(fn));
}

[[nodiscard]] inline bool is_equality_operator_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_equal") && fn.params.size() == 2 &&
           is_member_receiver_self_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_inequality_operator_function(const Function& fn) {
    return !fn.member_owner_class.empty() && fn.name.ends_with("_operator_not_equal") && fn.params.size() == 2 &&
           is_member_receiver_self_param(fn.params[0].type, fn.member_owner_class);
}

[[nodiscard]] inline bool is_equality_like_operator_function(const Function& fn) {
    return is_equality_operator_function(fn) || is_inequality_operator_function(fn);
}

[[nodiscard]] inline bool is_defaulted_equality_operator_function(const Function& fn) {
    return fn.is_defaulted && is_equality_like_operator_function(fn);
}

[[nodiscard]] inline std::string equality_operator_method_name(BinaryOp op) {
    switch (op) {
        case BinaryOp::Eq: return "operator_equal";
        case BinaryOp::Ne: return "operator_not_equal";
        default: return "";
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

struct StructField {
    SourceLocation loc;
    Type type;
    std::string name;
    std::optional<Initializer> default_initializer;
    AccessSpecifier access = AccessSpecifier::Public;
    std::vector<AlignmentSpecifier> alignment_specs;
    std::uint64_t resolved_alignment = 0;
};

struct StructDef {
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

struct ClassField {
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
struct ClassDef {
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

    [[nodiscard]] const BaseSpecifier* direct_ordinary_base() const {
        for (const BaseSpecifier& base : base_specifiers) {
            if (base.kind != BaseClassKind::Interface) return &base;
        }
        return nullptr;
    }

    [[nodiscard]] BaseSpecifier* direct_ordinary_base() {
        for (BaseSpecifier& base : base_specifiers) {
            if (base.kind != BaseClassKind::Interface) return &base;
        }
        return nullptr;
    }
};

// ch05 §5.11: one requirement inside a `concept Name = requires(...) {
// ... };` body -- restricted (a pragmatic v0.1 scoping cut, matching the
// spec's own examples) to a method call on the requires-expression's own
// placeholder parameter: `{ placeholder.method(args) };` (simple -- see
// has_return_constraint below) or `{ placeholder.method(args) } ->
// std::same_as<T>;` (compound, exact-type only, never
// std::convertible_to -- ch05 §5.11's own reasoning: scpp has no
// implicit scalar conversions at all, so the two would mean the same
// thing anyway). Arbitrary expressions, type-requirements
// (`typename T::Foo;`), and nested requirements (arbitrary boolean
// constant-expressions) are explicitly out of scope for v0.1.
struct ConceptRequirement {
    std::string method_name;
    // The call's own argument types (e.g. `f(x)` where `x: int` ->
    // {int}) -- excludes the implicit receiver (the placeholder itself),
    // exactly like Function::params excludes nothing but `this` is
    // always params[0] elsewhere; here there is no receiver slot at all
    // since the placeholder is never itself part of this list.
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
struct ConceptDef {
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

struct EnumVariant {
    std::string name;
    long long value = 0;
};

struct EnumDef {
    std::string name;
    Type underlying_type = named_type("int");
    std::vector<EnumVariant> variants;
    std::vector<std::string> namespace_path;
    bool is_exported = false;
    bool is_compile_time_dependency = false;
    std::string owning_module;
};

// ch11 §11.8: one `import name;` / `export import name;` declaration,
// or (ch11 §11.4) a same-module partition import (`import :part;` /
// `export import :part;`).
struct ImportDecl {
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

struct Program {
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

[[nodiscard]] inline const GlobalVar* find_visible_global(const Program* program, const std::vector<std::string>& namespace_path,
                                                          const std::string& name, bool explicit_global_qualification = false) {
    if (program == nullptr) return nullptr;
    auto matches_name = [&](const GlobalVar& global, std::string_view candidate) {
        return global.decl != nullptr && global.decl->var_name == candidate;
    };
    if (explicit_global_qualification) {
        for (const GlobalVar& global : program->globals) {
            if (matches_name(global, name)) return &global;
        }
        return nullptr;
    }
    for (std::size_t depth = namespace_path.size(); depth > 0; depth--) {
        std::string candidate;
        for (std::size_t i = 0; i < depth; i++) {
            if (!candidate.empty()) candidate += "::";
            candidate += namespace_path[i];
        }
        candidate += "::";
        candidate += name;
        for (const GlobalVar& global : program->globals) {
            if (matches_name(global, candidate)) return &global;
        }
    }
    for (const GlobalVar& global : program->globals) {
        if (matches_name(global, name)) return &global;
    }
    return nullptr;
}

struct TargetLayoutInfo {
    std::uint64_t pointer_size_bytes = sizeof(void*);
    std::uint64_t pointer_align_bytes = alignof(void*);
};

struct TypeLayoutInfo {
    std::uint64_t size_bytes = 0;
    std::uint64_t abi_align_bytes = 1;
};

[[nodiscard]] inline std::optional<TypeLayoutInfo> layout_of_type(const Program& program, const Type& type,
                                                                  TargetLayoutInfo target = {}) {
    struct LayoutComputer {
        const Program& program;
        TargetLayoutInfo target;
        std::unordered_set<std::string> visiting_named_types;

        [[nodiscard]] static std::uint64_t align_up(std::uint64_t value, std::uint64_t align) {
            if (align <= 1) return value;
            return ((value + align - 1) / align) * align;
        }

        [[nodiscard]] const EnumDef* find_enum(std::string_view name) const {
            for (const EnumDef& def : program.enums) {
                if (def.name == name) return &def;
            }
            return nullptr;
        }

        [[nodiscard]] const StructDef* find_struct(std::string_view name) const {
            const StructDef* forward_decl = nullptr;
            for (const StructDef& def : program.structs) {
                if (def.name != name) continue;
                if (!def.is_forward_declaration) return &def;
                if (forward_decl == nullptr) forward_decl = &def;
            }
            return forward_decl;
        }

        [[nodiscard]] const ClassDef* find_class(std::string_view name) const {
            const ClassDef* forward_decl = nullptr;
            for (const ClassDef& def : program.classes) {
                if (def.name != name) continue;
                if (!def.is_forward_declaration) return &def;
                if (forward_decl == nullptr) forward_decl = &def;
            }
            return forward_decl;
        }

        [[nodiscard]] std::optional<TypeLayoutInfo> named_scalar_layout(std::string_view name) const {
            if (name == "bool" || name == "char" || name == "int8_t" || name == "uint8_t") return TypeLayoutInfo{1, 1};
            if (name == "int16_t" || name == "uint16_t") return TypeLayoutInfo{2, 2};
            if (name == "int" || name == "unsigned int" || name == "int32_t" || name == "uint32_t" ||
                name == "float" || name == "float32_t") {
                return TypeLayoutInfo{4, 4};
            }
            if (name == "long" || name == "unsigned long" || name == "int64_t" || name == "uint64_t" ||
                name == "double" || name == "float64_t") {
                return TypeLayoutInfo{8, 8};
            }
            if (name == "size_t" || name == "ptrdiff_t") {
                std::uint64_t align = std::max<std::uint64_t>(target.pointer_align_bytes, 1);
                return TypeLayoutInfo{target.pointer_size_bytes, align};
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<TypeLayoutInfo> operator()(const Type& current) {
            switch (current.kind) {
                case TypeKind::Pointer:
                case TypeKind::Reference:
                case TypeKind::FunctionPointer: {
                    std::uint64_t align = std::max<std::uint64_t>(target.pointer_align_bytes, 1);
                    if ((current.kind == TypeKind::Pointer || current.kind == TypeKind::Reference) && current.pointee &&
                        current.pointee->kind == TypeKind::Named) {
                        if (const ClassDef* referent = find_class(current.pointee->name);
                            referent != nullptr && referent->is_interface) {
                            return TypeLayoutInfo{target.pointer_size_bytes * 2, align};
                        }
                    }
                    return TypeLayoutInfo{target.pointer_size_bytes, align};
                }
                case TypeKind::Function:
                    return std::nullopt;
                case TypeKind::Span: {
                    std::uint64_t pointer_align = std::max<std::uint64_t>(target.pointer_align_bytes, 1);
                    std::uint64_t count_align = 8;
                    std::uint64_t size = align_up(target.pointer_size_bytes, count_align) + 8;
                    return TypeLayoutInfo{align_up(size, std::max(pointer_align, count_align)),
                                          std::max(pointer_align, count_align)};
                }
                case TypeKind::Array: {
                    if (!current.element || current.array_size < 0) return std::nullopt;
                    std::optional<TypeLayoutInfo> element = (*this)(*current.element);
                    if (!element.has_value()) return std::nullopt;
                    return TypeLayoutInfo{element->size_bytes * static_cast<std::uint64_t>(current.array_size),
                                          element->abi_align_bytes};
                }
                case TypeKind::Named: {
                    if (current.name == "void") return std::nullopt;
                    if (std::optional<TypeLayoutInfo> scalar = named_scalar_layout(current.name)) return scalar;
                    if (const EnumDef* def = find_enum(current.name)) return (*this)(def->underlying_type);
                    if (visiting_named_types.contains(current.name)) return std::nullopt;
                    visiting_named_types.insert(current.name);
                    auto clear_visit = [&]() { visiting_named_types.erase(current.name); };
                    if (const StructDef* def = find_struct(current.name)) {
                        if (def->is_forward_declaration) {
                            clear_visit();
                            return std::nullopt;
                        }
                        if (!def->is_union) {
                            std::uint64_t offset = 0;
                            std::uint64_t overall_align = 1;
                            for (const StructField& field : def->fields) {
                                std::optional<TypeLayoutInfo> field_layout = (*this)(field.type);
                                if (!field_layout.has_value()) {
                                    clear_visit();
                                    return std::optional<TypeLayoutInfo>{};
                                }
                                std::uint64_t field_align =
                                    def->is_packed ? std::uint64_t{1}
                                                   : std::max(field_layout->abi_align_bytes, field.resolved_alignment);
                                offset = align_up(offset, field_align);
                                offset += field_layout->size_bytes;
                                overall_align = std::max(overall_align, field_align);
                            }
                            overall_align = def->is_packed ? 1 : std::max(overall_align, def->resolved_alignment);
                            clear_visit();
                            return TypeLayoutInfo{align_up(offset, overall_align), overall_align};
                        }
                        if (def->fields.empty()) {
                            clear_visit();
                            return std::nullopt;
                        }
                        std::uint64_t max_size = 0;
                        std::uint64_t overall_align = 1;
                        for (const StructField& field : def->fields) {
                            std::optional<TypeLayoutInfo> field_layout = (*this)(field.type);
                            if (!field_layout.has_value()) {
                                clear_visit();
                                return std::optional<TypeLayoutInfo>{};
                            }
                            max_size = std::max(max_size, field_layout->size_bytes);
                            overall_align = std::max(
                                overall_align,
                                def->is_packed ? std::uint64_t{1}
                                               : std::max(field_layout->abi_align_bytes, field.resolved_alignment));
                        }
                        overall_align = def->is_packed ? 1 : std::max(overall_align, def->resolved_alignment);
                        clear_visit();
                        return TypeLayoutInfo{align_up(max_size, overall_align), overall_align};
                    }
                    if (const ClassDef* def = find_class(current.name)) {
                        if (def->is_forward_declaration) {
                            clear_visit();
                            return std::nullopt;
                        }
                        std::uint64_t offset = 0;
                        std::uint64_t overall_align = 1;
                        if (const BaseSpecifier* base = def->direct_ordinary_base()) {
                            std::optional<TypeLayoutInfo> base_layout = (*this)(base->base_type);
                            if (!base_layout.has_value()) {
                                clear_visit();
                                return std::optional<TypeLayoutInfo>{};
                            }
                            offset = base_layout->size_bytes;
                            overall_align = std::max(overall_align, base_layout->abi_align_bytes);
                        }
                        // Mirrors Codegen::declare_class's `has_ordinary_vtable &&
                        // llvm_field_types.empty()` check: every non-interface class
                        // gets an implicit leading vtable pointer *unless* an ordinary
                        // base already contributed one (offset > 0 here means the base
                        // already supplied at least a vtable pointer and/or fields).
                        // Omitting this made sizeof()/alignof() disagree with the real,
                        // ABI object size for any class with a vtable (i.e. virtually
                        // every class, since a virtual destructor is mandatory --
                        // ch11 §11.5(1)), silently under-reporting size by one pointer.
                        if (!def->is_interface && offset == 0) {
                            offset = target.pointer_size_bytes;
                            overall_align = std::max(overall_align, target.pointer_align_bytes);
                        }
                        for (const ClassField& field : def->fields) {
                            std::optional<TypeLayoutInfo> field_layout = (*this)(field.type);
                            if (!field_layout.has_value()) {
                                clear_visit();
                                return std::optional<TypeLayoutInfo>{};
                            }
                            std::uint64_t field_align = std::max(field_layout->abi_align_bytes, field.resolved_alignment);
                            offset = align_up(offset, field_align);
                            offset += field_layout->size_bytes;
                            overall_align = std::max(overall_align, field_align);
                        }
                        overall_align = std::max(overall_align, def->resolved_alignment);
                        clear_visit();
                        return TypeLayoutInfo{align_up(offset, overall_align), overall_align};
                    }
                    clear_visit();
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }
    };

    return LayoutComputer{program, target, {}}(type);
}

} // namespace scpp
