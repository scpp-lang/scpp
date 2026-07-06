module;

#include <algorithm>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module scpp.movecheck;

import scpp.ast;
import scpp.mir;

export namespace scpp {

struct DataflowError : std::runtime_error {
    explicit DataflowError(const std::string& message, SourceLocation loc = {})
        : std::runtime_error(message), loc(loc) {}
    SourceLocation loc;
};

} // namespace scpp

namespace scpp {
namespace {

// The lattice tracked per local variable:
//   Bottom      -- not currently in scope on this path: either not yet
//                  declared here (e.g. a name declared only in a sibling
//                  `if` branch), or already gone out of lexical scope (a
//                  `ScopeExit` MIR statement resets a local back to this
//                  state at the end of its enclosing block/if-branch/
//                  while-body, mirroring codegen's own scope_stack_).
//   Initialized -- holds a valid, readable value. scpp has no concept of an
//                  "uninitialized" scalar: every local without an explicit
//                  initializer is zero-initialized (0 / false / null / all-
//                  zero fields) by codegen, for every type, so a `Declare`
//                  MIR statement always yields Initialized, never a
//                  separate "not yet given a value" state. For a reference
//                  (`T&`/`const T&`), Initialized means "currently bound
//                  and in scope" -- see `BindReference` below.
//   MovedOut    -- was holding a unique_ptr value that has been moved away
//   Conflict    -- different incoming paths disagree (e.g. Initialized on
//                  one, MovedOut on another) -- treated as unsafe to read,
//                  same as Bottom/MovedOut.
// Only `Initialized` is safe to read; every other state is rejected (with a
// tailored message) if the checker sees a use of it.
enum class LocalState { Bottom, Initialized, MovedOut, Conflict };

using StateMap = std::unordered_map<std::string, LocalState>;

// Per-*root-place* borrow bookkeeping for alias-XOR-mutability (ch05.2). A
// "root place" is a plain local/parameter that isn't itself tracked as
// borrowing something else (see `resolve_root_place`): a chain of
// reference-to-reference bindings is flattened so every borrow is always
// checked/recorded against the one real place ultimately being aliased,
// not an intermediate reference's name.
struct BorrowState {
    int shared_count = 0;
    bool mutable_borrow = false;

    bool operator==(const BorrowState&) const = default;
};

using BorrowMap = std::unordered_map<std::string, BorrowState>;

// Maps a currently-bound reference local/parameter's name directly to the
// *root* place it (transitively) borrows -- see `resolve_root_place`.
using RefTargetMap = std::unordered_map<std::string, std::string>;

// Every struct/class's own declared field name -> Type, across the whole
// program -- see DataflowState::class_field_types' own comment for why
// this exists (movecheck's Body-only architecture otherwise has no way
// to resolve a Member expression's type).
using ClassFieldTypes = std::unordered_map<std::string, std::unordered_map<std::string, Type>>;

// The full per-program-point dataflow state: ownership/move state per
// local (`locals`), active borrows per root place (`borrows`), which root
// each currently-bound reference aliases (`ref_targets`), and the current
// `unsafe { }` nesting depth (`unsafe_depth`). Bundled into one struct so
// the worklist algorithm in check_function can thread (and join) all of
// it together as a single unit.
struct DataflowState {
    StateMap locals;
    BorrowMap borrows;
    RefTargetMap ref_targets;
    // Ch01 §1.3's nesting counter: incremented by UnsafeEnter, decremented
    // by UnsafeExit (see apply_statement). Zero means "not currently
    // licensed to relax ch05.5's checks"; greater than zero means inside
    // a lexical `unsafe { }` block. Every function starts at zero (ch01:
    // checking is the unconditional default, with no per-function way to
    // start already-unsafe) -- see check_function's entry_state setup.
    // Unlike `locals`/`borrows`/`ref_targets`, this never needs real join
    // semantics (see join_states): it's a purely lexical/structural fact,
    // identical via every predecessor path to a given program point.
    int unsafe_depth = 0;
    // ch04 §4.2/ch05 §5.9: the class `this` belongs to, if the function
    // currently being checked is a method (`params[0].name == "this"`) --
    // empty otherwise. Set once in check_function from the function's own
    // params[0], if present, and never changes afterward -- exactly like
    // `unsafe_depth` above (a purely structural fact, identical via every
    // predecessor path), so join_states doesn't need real join logic here
    // either.
    std::string current_class;
    // All class names in the whole program (ch04 §4.2), built once by
    // check_moves and never mutated afterward -- used only to tell a
    // class-typed Member base (access-controlled: private-by-construction
    // fields, ch04 §4.2) apart from a struct-typed one (never access-
    // controlled, ch04 §4.1). A raw, non-owning pointer (not a copy)
    // since check_moves's own local set outlives every DataflowState
    // that references it; never null once check_function sets it (may be
    // empty, for a program with no classes at all).
    const std::unordered_set<std::string>* class_names = nullptr;
    // Every struct/class's own field name -> declared Type, across the
    // whole program -- built once by check_moves (mirrors class_names'
    // identical lifetime/non-null-once-set contract). The *only* way
    // movecheck's otherwise Body-only (no Program access) machinery can
    // resolve a `this.field`/`obj.field` Member expression's own type --
    // needed by validate_deref_operand/apply_deref to validate `*p`
    // through a captured std::unique_ptr *field* (ch05 §5.12's own
    // `[p = std::move(p)]` init-capture example: after the closure's
    // field-access rewrite, a captured name is a Member, not a plain
    // Identifier, the only shape that lookup previously handled).
    const ClassFieldTypes* class_field_types = nullptr;
    // Where the statement/expression currently being processed begins in
    // the source (see SourceLocation, ast.cppm) -- refreshed by
    // apply_statement/apply_expr as they recurse, so any DataflowError
    // thrown along the way can report a location. Purely a diagnostics
    // aid, carrying no dataflow meaning whatsoever -- deliberately
    // excluded from operator== below (see the fixed-point worklist loop
    // in check_function, which relies on out-state equality to detect
    // convergence): including it would compare *where the last-processed
    // statement happened to be*, not any actual move/borrow fact, which
    // has no business influencing whether the analysis has reached a
    // fixed point.
    SourceLocation current_loc;

    [[nodiscard]] bool operator==(const DataflowState& other) const {
        return locals == other.locals && borrows == other.borrows && ref_targets == other.ref_targets &&
               unsafe_depth == other.unsafe_depth && current_class == other.current_class &&
               class_names == other.class_names && class_field_types == other.class_field_types;
    }
};

// A function's checked signature, built once for the whole Program by
// check_moves. Needed for call-site reference-parameter binding (see
// apply_reference_argument), resolving/validating a reference return
// value against spec ch05.3's elision rule (see resolve_elided_param_index
// and check_terminator's Return case), and gating the "callee must be
// wrapped in `unsafe { }`" check (ch02/ch05.5) in check_call_arguments.
struct FunctionSignature {
    std::vector<Type> param_types;
    Type return_type;
    // Set only when return_type is itself a Reference: the index into
    // param_types of the sole reference parameter this function's own
    // returned reference must (transitively) resolve back to, and that
    // a caller's argument for it is assumed to alias for the duration
    // of a call to this function returning a reference derived from
    // that argument (see resolve_borrow_source_root's Call case).
    // Computed once by resolve_elided_param_index, which throws if
    // return_type is a Reference but no valid elision exists.
    std::optional<size_t> elided_param_index;
    // Mirrors Function::is_extern_c. Every ordinary scpp function is
    // checked by default (ch01) and needs no `unsafe { }` to call; an
    // `extern "C"` *declaration* (no body -- its real implementation is
    // never seen by any scpp compiler) is the one remaining always-
    // unchecked callee category, so calling it is rejected unless the
    // call site's DataflowState::unsafe_depth is greater than zero
    // (ch01 §1.3/ch02) -- see check_call_arguments.
    bool is_extern_c = false;
    // Where this specific overload's declaration begins -- see
    // Function::loc; needed for diagnostics that are about one
    // particular overload (e.g. a redefinition error naming the
    // conflicting declaration).
    SourceLocation loc;
};

// ch05 §5.10: a name (free function or method, post class-mangling --
// see resolve_callee_signature) may now have *multiple* FunctionSignature
// entries -- one per overload -- instead of exactly one. Resolving a
// specific call to the single matching overload (exact type match only,
// ch06: no scpp scalar type implicitly converts to any other) is
// resolve_overload's job; nothing here assumes "one entry per name" any
// longer.
using Signatures = std::unordered_map<std::string, std::vector<FunctionSignature>>;

LocalState join(LocalState a, LocalState b) {
    if (a == b) return a;
    if (a == LocalState::Bottom) return b;
    if (b == LocalState::Bottom) return a;
    return LocalState::Conflict;
}

// Joins two per-block state snapshots (e.g. the OUT states of two
// predecessors flowing into a shared successor block).
StateMap join_maps(const StateMap& a, const StateMap& b) {
    StateMap result = a;
    for (const auto& [name, state] : b) {
        auto it = result.find(name);
        result[name] = it == result.end() ? state : join(it->second, state);
    }
    return result;
}

// Conservatively merges two borrow snapshots for the same root place: if
// the incoming paths disagree, pick the *more restrictive* combination
// (mutable if either says mutable; the larger shared count) rather than
// silently under-restricting. In well-formed programs this should always
// be a same-value merge in practice, since every borrow is released (via
// ScopeExit) no later than the end of its own lexically-nested scope, so
// it can't still be "half alive" at a join point coming from only one
// predecessor -- see the BorrowState/ScopeExit comments below.
BorrowState join_borrow(const BorrowState& a, const BorrowState& b) {
    BorrowState result;
    result.mutable_borrow = a.mutable_borrow || b.mutable_borrow;
    result.shared_count = std::max(a.shared_count, b.shared_count);
    return result;
}

BorrowMap join_borrow_maps(const BorrowMap& a, const BorrowMap& b) {
    BorrowMap result = a;
    for (const auto& [place, borrow] : b) {
        auto it = result.find(place);
        result[place] = it == result.end() ? borrow : join_borrow(it->second, borrow);
    }
    return result;
}

// A reference is bound exactly once and never rebound (ch03), so in a
// well-formed program every incoming path agrees on what a given
// reference name targets; last-write-wins is just a harmless tie-break
// for whatever a not-yet-rejected, malformed program's fixed-point
// iteration computes along the way.
RefTargetMap join_ref_targets(const RefTargetMap& a, const RefTargetMap& b) {
    RefTargetMap result = a;
    for (const auto& [ref_name, root] : b) {
        result.insert_or_assign(ref_name, root);
    }
    return result;
}

DataflowState join_states(const DataflowState& a, const DataflowState& b) {
    return DataflowState{
        join_maps(a.locals, b.locals),
        join_borrow_maps(a.borrows, b.borrows),
        join_ref_targets(a.ref_targets, b.ref_targets),
        // In a well-formed program every incoming path agrees on the
        // unsafe nesting depth at a given program point (see
        // DataflowState::unsafe_depth) -- min is just a conservative,
        // defensive tie-break (fail toward "not unsafe", i.e. checks stay
        // on) for whatever a not-yet-rejected, malformed program's
        // fixed-point iteration computes along the way, mirroring
        // join_ref_targets' own comment above.
        std::min(a.unsafe_depth, b.unsafe_depth),
        // `current_class`/`class_names`/`class_field_types` are set once
        // per function and never change afterward (see DataflowState's
        // own comments) -- identical on both sides in a well-formed
        // program, so simply keeping `a`'s is enough (no real join
        // needed, same reasoning as `unsafe_depth` just above, minus the
        // "fail safe" tie-break since there's no meaningful direction to
        // fail toward here).
        a.current_class,
        a.class_names,
        a.class_field_types,
        // `current_loc` carries no dataflow meaning at all (see its own
        // comment on DataflowState) and is excluded from operator==, so
        // which side's value ends up here doesn't affect correctness --
        // apply_statement immediately overwrites it for whichever
        // statement runs next anyway. Keeping `a`'s is just the same
        // "no real join needed" shape as every other field above, not a
        // deliberate choice between the two.
        a.current_loc,
    };
}

std::string describe_bad_state(const std::string& name, LocalState state) {
    switch (state) {
        case LocalState::MovedOut:
            return "use of moved-out variable '" + name + "'";
        case LocalState::Conflict:
            return "use of variable '" + name +
                   "' whose initialization state is inconsistent across incoming control-flow paths "
                   "(initialized on some, not on others)";
        case LocalState::Bottom:
            return "use of variable '" + name + "' that is out of scope here";
        case LocalState::Initialized:
        default:
            return "use of possibly-uninitialized variable '" + name + "'";
    }
}

[[nodiscard]] bool is_unique_ptr(const Type& type) { return type.kind == TypeKind::UniquePtr; }
[[nodiscard]] bool is_reference(const Type& type) { return type.kind == TypeKind::Reference; }
[[nodiscard]] bool is_span(const Type& type) { return type.kind == TypeKind::Span; }

// Returns the class/struct name `type` resolves to as a Named type,
// seeing through a Reference (e.g. `this`'s own declared type, ch05
// §5.9) -- or empty if `type` isn't (possibly a reference to) a Named
// type at all. Used only by apply_expr's Member case, to tell a
// class-typed base (access-controlled, ch04 §4.2) apart from a
// struct-typed one (never access-controlled, ch04 §4.1).
[[nodiscard]] std::string named_type_name(const Type& type) {
    if (type.kind == TypeKind::Named) return type.name;
    if (type.kind == TypeKind::Reference && type.pointee->kind == TypeKind::Named) return type.pointee->name;
    return "";
}

// Structural (deep) equality between two Types -- needed since Type's
// pointee/element are shared_ptr (Type's own comment: "so Type stays
// copyable"), so two independently-parsed-but-conceptually-identical
// types (e.g. two separate `int*` parameter declarations) are different
// shared_ptr instances and would compare unequal under a naively
// `=default`-ed operator==. Used only for function-overload resolution
// (ch05 §5.10): since ch06 established no scpp scalar type implicitly
// converts to any other, overload resolution is exact type match only --
// this is that "exact match" test. Deliberately requires is_mutable_ref/
// is_mutable_pointee to also match: `T&` and `const T&` (or `T*`/
// `const T*`) are distinct parameter types for overloading purposes, not
// interchangeable. Reference additionally requires is_rvalue_ref to
// match: `T&`/`const T&` (a borrow) and `T&&` (ch03's move-parameter
// form) are likewise distinct parameter types, never interchangeable --
// meaningless for Span (which has no rvalue-reference concept at all).
[[nodiscard]] bool types_equal(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case TypeKind::Named:
            return a.name == b.name;
        case TypeKind::Pointer:
            return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
        case TypeKind::UniquePtr:
            return types_equal(*a.pointee, *b.pointee);
        case TypeKind::Reference:
            return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Span:
            return a.is_mutable_ref == b.is_mutable_ref && types_equal(*a.pointee, *b.pointee);
        case TypeKind::Array:
            return a.array_size == b.array_size && types_equal(*a.element, *b.element);
    }
    return false;
}

// A Call expression's signature-lookup key, plus how many leading
// `signatures[key].param_types` entries are already spoken for before
// `expr.args[0]` (1 when an implicit `this` occupies param_types[0], 0
// otherwise).
struct CalleeSignature {
    std::string key;
    size_t param_offset = 0;
};

// Resolves a Call expression's signature-lookup key, accounting for a
// method call's receiver (ch04 §4.2/ch05 §5.9): `obj.name(...)`/
// `this->name(...)` stores its receiver in `call_expr.lhs` and only the
// unqualified method name in `call_expr.name`, but `signatures` (like
// codegen's own `module_->getFunction`) is keyed by the synthesized
// `ClassName_methodName` form (see parse_class_def) -- exactly like
// codegen_call independently resolves the same fact from the receiver's
// type. Scoped to a plain Identifier receiver (covers `this->method()`
// and `obj.method()` for a local/parameter `obj`) or a Lambda literal
// receiver (ch05 §5.12's IIFE, e.g. `[](int x){...}(5)` -- already
// resolved to its own synthesized closure class name by the time
// check_moves runs, see monomorphize_generics) -- the only two shapes
// movecheck can resolve a type for without a real type-checker) -- a
// more complex receiver expression falls back to the unqualified name
// and a zero offset, same as an ordinary free-function call. Shared by
// check_call_arguments and produces_unique_ptr_rvalue so both resolve a
// method call's callee identically.
[[nodiscard]] CalleeSignature resolve_callee_signature(const Expr& call_expr, const Body& body) {
    if (call_expr.lhs) {
        std::string class_name;
        if (call_expr.lhs->kind == ExprKind::Identifier) {
            auto type_it = body.local_types.find(call_expr.lhs->name);
            if (type_it != body.local_types.end()) class_name = named_type_name(type_it->second);
        } else if (call_expr.lhs->kind == ExprKind::Lambda && !call_expr.lhs->name.empty()) {
            class_name = call_expr.lhs->name;
        }
        if (!class_name.empty()) return CalleeSignature{class_name + "_" + call_expr.name, 1};
    }
    return CalleeSignature{call_expr.name, 0};
}

// Forward declarations for a small mutually-recursive group implementing
// ch05 §5.10's function-overload resolution:
//  - infer_expr_type needs resolve_overload for a nested Call argument's
//    own return type.
//  - resolve_overload needs argument_matches_parameter to test each
//    candidate, which in turn needs infer_expr_type (to compare argument/
//    parameter types), produces_unique_ptr_rvalue (defined below, to
//    validate a std::unique_ptr-typed parameter's argument), and
//    is_read_only_reachable (defined much further below, for the
//    T&-beats-const-T&-for-a-mutable-lvalue tie-break).
// All of this always terminates: every recursive step is into a strictly
// smaller sub-expression.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool produces_unique_ptr_rvalue(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                            const Signatures& signatures);

// Whether `arg` is a legitimate argument for a candidate overload's
// parameter declared as `param_type`, for exact-type-match overload
// resolution (ch05 §5.10) -- not a full validity check (that's
// check_call_arguments/apply_reference_argument's job, once a specific
// overload has already been picked); this only needs to decide which of
// several candidates is *the* match.
[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                                const Signatures& signatures) {
    if (is_unique_ptr(param_type)) {
        // ch05 §5.1/§5.10: std::unique_ptr is scpp's one move-restricted
        // type -- a bare lvalue is never a legitimate by-value (owning)
        // argument for it (that would be an implicit, unmarked move, ch05
        // §5.1); only std::move(x)/std::make_unique<T>(...) are.
        if (!produces_unique_ptr_rvalue(arg, body, signatures)) return false;
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && arg_type->kind == TypeKind::UniquePtr &&
               types_equal(*arg_type->pointee, *param_type.pointee);
    }
    if (is_reference(param_type) && param_type.is_rvalue_ref) {
        // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the mirror image of
        // the ordinary-reference case just below: needs a genuine
        // rvalue-producing argument, never a bare place.
        return produces_rvalue_of_type(arg, *param_type.pointee, body, signatures);
    }
    if (is_reference(param_type)) {
        // A bare lvalue-like place (Identifier/Member/Subscript/a
        // unique_ptr or raw pointer's Deref -- the same shapes
        // resolve_borrow_source_root accepts as a borrow source) is
        // viable against a T&/const T& parameter; std::move/MakeUnique/a
        // literal never is (there's no place to borrow from).
        if (arg.kind == ExprKind::Move || arg.kind == ExprKind::MakeUnique ||
            arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::BoolLiteral ||
            arg.kind == ExprKind::CharLiteral || arg.kind == ExprKind::StringLiteral) {
            return false;
        }
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && types_equal(*arg_type, *param_type.pointee);
    }
    // By-value scalar/struct parameter (ch04 §4.1: freely, implicitly
    // copyable) -- unlike std::unique_ptr above, an ordinary lvalue
    // argument is just as viable as any rvalue here (a plain copy); no
    // std::move required. (A class-typed by-value parameter can't occur
    // at all -- check_function's own by-value-parameter rejection, ch04
    // §4.2 -- so this path is only ever reached for a scalar/struct T.)
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    return arg_type.has_value() && types_equal(*arg_type, param_type);
}

// Resolves `call_expr` to the single FunctionSignature (among possibly
// several overloads sharing `callee.key`'s name) whose parameters match
// this call's actual arguments (ch05 §5.10) -- exact type match only, so
// resolution never needs a conversion-ranking algorithm. Returns nullptr
// when no candidate matches (the caller reports a clear "no matching
// overload" diagnostic) or when `callee.key` names nothing at all.
//
// When strictly more than one candidate matches (only possible via the
// by-value/by-reference axis -- two overloads can never share an
// identical parameter-type list, ch05 §5.10), applies the "T& beats
// const T& for a mutable lvalue" tie-break (reused from real C++,
// resolving the const/non-const method-overloading case, e.g.
// get()/get() const) across every reference-typed parameter position
// (including an implicit `this`, ch05 §5.9) where the matches disagree
// on mutability. If that still doesn't produce a unique winner, this is
// a genuine ambiguity this version has no further tie-break for --
// falls back to the first match found (in declaration order) rather
// than crashing, since v0.1's scalar-only overload sets make actually
// reaching this exceedingly rare in a real, well-formed program.
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures) {
    auto it = signatures.find(callee.key);
    if (it == signatures.end()) return nullptr;
    // The overwhelmingly common case: exactly one function has ever been
    // declared under this name, so there's nothing to *disambiguate*
    // between -- return it unconditionally, without running any of the
    // exact-type-match/this-mutability machinery below at all. This
    // matters beyond just being a harmless shortcut: infer_expr_type
    // can't resolve every expression shape (Member/Subscript chains,
    // notably -- movecheck has no Program access to their field/element
    // types), so *requiring* a successful match here would wrongly break
    // an ordinary, non-overloaded call whose argument happens to be one
    // of those shapes (e.g. `f(obj.field)`) purely because overload
    // resolution can't prove a match, not because one doesn't exist.
    // Whether this one candidate's parameters actually fit the call's
    // arguments is left to the checks that already existed before
    // overloading (apply_reference_argument, codegen's own type
    // checking, ...), exactly as before this feature.
    if (it->second.size() == 1) return &it->second[0];

    std::vector<const FunctionSignature*> matches;
    for (const FunctionSignature& candidate : it->second) {
        if (candidate.param_types.size() != call_expr.args.size() + callee.param_offset) continue;
        bool all_match = true;
        // The receiver (`this`), for a method call: viable only if the
        // candidate's own `this` mutability doesn't demand more than the
        // receiver place can actually provide (mirrors
        // apply_reference_argument's identical mutable-vs-read-only-
        // reachable check, applied here purely for resolution purposes).
        if (callee.param_offset == 1 && call_expr.lhs && candidate.param_types[0].is_mutable_ref &&
            is_read_only_reachable(*call_expr.lhs, body, signatures)) {
            all_match = false;
        }
        for (size_t i = 0; all_match && i < call_expr.args.size(); i++) {
            all_match = argument_matches_parameter(*call_expr.args[i], candidate.param_types[i + callee.param_offset],
                                                     body, signatures);
        }
        if (all_match) matches.push_back(&candidate);
    }

    if (matches.size() <= 1) return matches.empty() ? nullptr : matches[0];

    // Tie-break: prefer whichever match has the most mutable-reference
    // parameters among positions where the argument is itself a mutable
    // place (including `this`, checked the same way as above) -- the
    // higher-scoring candidate is the more "specific" one a mutable
    // argument licenses, exactly like real C++'s own T&-over-const-T&
    // preference.
    auto mutable_ref_score = [&](const FunctionSignature& candidate) {
        int score = 0;
        if (callee.param_offset == 1 && call_expr.lhs && candidate.param_types[0].is_mutable_ref &&
            !is_read_only_reachable(*call_expr.lhs, body, signatures)) {
            score++;
        }
        for (size_t i = 0; i < call_expr.args.size(); i++) {
            const Type& param_type = candidate.param_types[i + callee.param_offset];
            if (is_reference(param_type) && param_type.is_mutable_ref &&
                !is_read_only_reachable(*call_expr.args[i], body, signatures)) {
                score++;
            }
        }
        return score;
    };
    const FunctionSignature* best = matches[0];
    int best_score = mutable_ref_score(*best);
    bool unique_best = true;
    for (size_t i = 1; i < matches.size(); i++) {
        int score = mutable_ref_score(*matches[i]);
        if (score > best_score) {
            best = matches[i];
            best_score = score;
            unique_best = true;
        } else if (score == best_score) {
            unique_best = false;
        }
    }
    return unique_best ? best : matches[0];
}

// Structurally validates and resolves spec ch05.3's elision rule for a
// single function's signature: if `fn.return_type` is a Reference, `fn`
// must have *exactly* one reference-typed parameter (this language has
// no `this`/method-receiver concept at all yet, so that half of the
// spec's elision rule never applies -- every reference-returning
// function is a free function, so its sole eligible parameter is
// whichever plain parameter happens to be a reference), and that
// parameter's mutability must be able to license the return type's: a
// `const T&` parameter can only license a `const T&` return, never a
// plain `T&` (that would manufacture a mutable alias out of a shared
// one). Throws DataflowError describing the mismatch otherwise; returns
// nullopt (meaning "elision doesn't apply") when `fn.return_type` isn't
// a Reference at all.
[[nodiscard]] std::optional<size_t> resolve_elided_param_index(const Function& fn) {
    if (!is_reference(fn.return_type)) return std::nullopt;

    std::optional<size_t> found;
    for (size_t i = 0; i < fn.params.size(); i++) {
        // ch03/ch05 §5.11: an rvalue-reference (`T&&`) parameter is
        // never an eligible elision source -- its argument may be a
        // fresh temporary (a literal, a std::make_unique<T>(...)/call
        // result) whose storage the caller never promises to keep alive
        // past the call, unlike an ordinary T&/const T& argument (always
        // a real place the caller keeps borrowed for the call's
        // duration). Returning a reference derived from it would be a
        // dangling reference in exactly the cases elision is supposed to
        // rule out.
        if (!is_reference(fn.params[i].type) || fn.params[i].type.is_rvalue_ref) continue;
        if (found.has_value()) {
            throw DataflowError(
                "function '" + fn.name +
                "' returns a reference but has more than one reference parameter; scpp v0.1 can only infer a "
                "returned reference's lifetime when there is exactly one (spec ch05.3) -- refactor to take a "
                "single reference parameter, or return by value/std::unique_ptr instead",
                fn.loc);
        }
        found = i;
    }
    if (!found.has_value()) {
        throw DataflowError(
            "function '" + fn.name +
            "' returns a reference but has no reference parameter to infer its lifetime from (spec ch05.3) -- "
            "refactor to take a single reference parameter, or return by value/std::unique_ptr instead",
            fn.loc);
    }
    if (fn.return_type.is_mutable_ref && !fn.params[*found].type.is_mutable_ref) {
        throw DataflowError("function '" + fn.name +
                             "' returns a mutable reference ('T&') but its sole reference parameter '" +
                             fn.params[*found].name +
                             "' is a shared reference ('const T&'); a mutable reference cannot be manufactured "
                             "from a shared one",
            fn.loc);
    }
    return found;
}

// Whether assigning through `expr` (used as an assignment's *target* --
// see apply_expr's Binary/Assign case) would write through a read-only
// (`const T&` reference, `std::span<const T>`, or `const T*` raw
// pointer) somewhere in its chain -- Reference/Span reuse the same
// `is_mutable_ref` flag for "is this view/borrow read-only", Pointer has
// its own analogous `is_mutable_pointee` flag (see ast.cppm's Type; ch05
// §5.7 -- writing through a `const T*` is an ordinary type error,
// unconditionally, even inside `unsafe { }`, so this is never gated by
// `state.unsafe_depth`). A `.field`/`[index]` projection's constness
// always comes from its outermost base (struct fields can never
// themselves be references or spans -- ch04.1 permanently forbids that
// -- so there's never a *nested* one to find deeper in the chain); a
// call's constness comes from its own return type. A plain local (not
// itself a reference/span) is always writable here -- move/
// initialization-state legality is checked separately, this is purely
// about const-ness.
[[nodiscard]] bool assignment_target_is_read_only(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            return it != body.local_types.end() && (is_reference(it->second) || is_span(it->second)) &&
                   !it->second.is_mutable_ref;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return assignment_target_is_read_only(*expr.lhs, body, signatures);
        case ExprKind::Unary: {
            // `*p = ...`/`p->x = ...` (`p` a raw pointer): read-only iff
            // `p`'s own declared type says `const T*`
            // (is_mutable_pointee == false) -- ch05 §5.7's "writing
            // through a const T* is an ordinary type error, in *every*
            // context, including inside unsafe { }" rule (this function
            // is never consulted with `state.unsafe_depth` at all, so
            // there is nothing here for `unsafe { }` to relax). A
            // std::unique_ptr pointee has no const-qualification concept
            // in this version (same reasoning as is_read_only_reachable),
            // so it's never read-only through this path.
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
            auto it = body.local_types.find(expr.lhs->name);
            return it != body.local_types.end() && it->second.kind == TypeKind::Pointer &&
                   !it->second.is_mutable_pointee;
        }
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            return sig != nullptr && is_reference(sig->return_type) && !sig->return_type.is_mutable_ref;
        }
        default:
            return false;
    }
}

// Finds the root place to check for an *outstanding borrow* when
// assigning through `expr` (used as an assignment target -- see
// apply_expr's Binary/Assign case), or nullopt if no such check is
// needed. Three different cases share the same `.field`/`[index]`/`*`
// chain shape, and must be told apart:
//   - `a.x = 1;` where `a` is a *plain place* (not itself a reference):
//     this writes directly to a's storage, so it must be rejected if
//     `a` currently has *any* outstanding borrow (mutable or shared) --
//     exactly like plain `a = 1;` already is (see apply_statement's
//     Assign case) -- since a live borrow (of *any* field, under this
//     version's whole-root conservatism, spec ch05.2) assumes the data
//     won't change underneath it. Returns `a` in this case.
//   - `r.x = 1;` where `r` is itself a reference (`T& r = ...;`) or
//     `s[i] = 1;` where `s` is a `std::span<T>`: both write *through* a
//     borrow, and holding a live *mutable* one is already the license to
//     do so (checked separately by assignment_target_is_read_only) --
//     its own root necessarily shows a borrow (its own), which would
//     cause a false rejection if checked here too, so this case returns
//     nullopt (no additional check).
//   - `(*p).x = 1;`/`p->x = 1;`/`*p = 1;` where `p` is a std::unique_ptr
//     or (inside unsafe {}) a raw pointer: this writes directly through
//     p's pointee storage, exactly like the plain-place Identifier case
//     above -- `p` itself is the root to check (same root
//     resolve_borrow_source_root's own Unary case uses for a *read*),
//     since a live borrow into `*p` (e.g. `const T& r = *p;`) must
//     forbid writing through `*p` while `r` is alive, the same as it
//     would for a plain struct field. Only Deref ever reaches here:
//     Neg/Not never produce an addressable place (codegen_lvalue's
//     default case already rejects them), so this is the only unary op
//     assignment-target parsing can produce; a non-Identifier operand
//     (e.g. some future more-general expression) conservatively returns
//     nullopt rather than guessing.
// A Call target (spec ch05.3's reference-returning functions) is
// likewise nullopt: its own mutability is checked by
// assignment_target_is_read_only, and there is no lasting root of its
// own left to re-check (its argument's borrow was already transient and
// released by the time the call returned).
[[nodiscard]] std::optional<std::string> direct_write_root(const Expr& expr, const Body& body) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it != body.local_types.end() && (is_reference(it->second) || is_span(it->second))) {
                return std::nullopt;
            }
            return expr.name;
        }
        case ExprKind::Member:
        case ExprKind::Subscript:
            return direct_write_root(*expr.lhs, body);
        case ExprKind::Unary:
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) {
                return std::nullopt;
            }
            return expr.lhs->name;
        default:
            return std::nullopt;
    }
}

// The expressions allowed to produce a std::unique_ptr rvalue: moving an
// existing one out, freshly heap-allocating one via std::make_unique
// (scpp has no `new` expression at all -- make_unique is the sole
// sanctioned allocation syntax, and is itself a compiler builtin rather
// than a real generic call, same treatment as std::move), or calling a
// function/method whose own return type is std::unique_ptr<T> -- exactly
// like real C++, a function's return value is already an rvalue at the
// call site and needs no std::move (only a *named* variable needs an
// explicit std::move to be treated as movable-from). Consistent with
// check_call_arguments, which already allows this same Call expression
// shape unconditionally when passed directly as another call's argument.
[[nodiscard]] bool produces_unique_ptr_rvalue(const Expr& expr, const Body& body, const Signatures& signatures) {
    if (expr.kind == ExprKind::Move || expr.kind == ExprKind::MakeUnique) return true;
    if (expr.kind == ExprKind::Call) {
        CalleeSignature callee = resolve_callee_signature(expr, body);
        const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
        return sig != nullptr && is_unique_ptr(sig->return_type);
    }
    return false;
}

// ch03/ch05 §5.11: the expressions allowed to bind to a `T&&` (rvalue-
// reference/move) parameter -- generalizes produces_unique_ptr_rvalue
// above to any type (not just std::unique_ptr), checked against a
// specific `expected_type` since, unlike a unique_ptr parameter (always
// disambiguated by its own pointee type elsewhere), an arbitrary T&&
// parameter's type is exactly what tells a legitimate argument from an
// ill-typed one here. Reused, via the same Type::is_rvalue_ref flag, for
// a `Concept auto&&` generic parameter's own witness-typed slot (ch05
// §5.11) and for passing a lambda expression literal to one (ch05
// §5.12, once ExprKind::Lambda exists -- add it to the switch below at
// that point; a lambda literal is a fresh prvalue exactly like the
// cases already handled here). `std::move(x)` is allowed regardless of
// x's own type (not just std::unique_ptr, generalizing the existing
// Move-processing restriction would be a much larger, cross-cutting
// change deferred past this round -- see apply_expr's Move case, which
// still rejects a non-unique_ptr std::move with a clear diagnostic, so
// this relaxation here is purely "which expression *shapes* are
// considered", not a silent widening of what std::move itself accepts).
// A bare place (Identifier/Member/Subscript/a pointer Deref) is never
// legitimate here: passing an existing lvalue directly into a by-move
// parameter without an explicit std::move would be exactly the
// unmarked implicit move ch05 §5.1 forbids -- the mirror image of
// argument_matches_parameter's ordinary-reference case, which rejects
// these same expression shapes for the opposite reason (there's no
// borrowable place to speak of).
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                            const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Move:
        case ExprKind::MakeUnique:
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::Lambda:
            // ch05 §5.12: a (by now resolved) lambda literal is a fresh
            // prvalue exactly like a literal or std::make_unique<T>(...)
            // -- the primary motivating case for a `Concept auto&&`
            // parameter (ch05 §5.11), e.g. passing a closure directly to
            // a generic function.
            break;
        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            // A reference-returning call yields a place/alias, not a
            // fresh value (see resolve_borrow_source_root's own Call
            // case) -- legitimate as a T&/const T& source elsewhere, but
            // not here.
            if (sig == nullptr || is_reference(sig->return_type)) return false;
            break;
        }
        default:
            return false;
    }
    std::optional<Type> actual_type = infer_expr_type(expr, body, signatures);
    return actual_type.has_value() && types_equal(*actual_type, expected_type);
}

// Infers `expr`'s scpp type, for function-overload resolution purposes
// only (ch05 §5.10) -- a best-effort, non-exhaustive type inference
// (movecheck has no general type-checking pass at all, by design: see
// e.g. produces_unique_ptr_rvalue's similarly-scoped Call handling just
// above). Covers every expression shape that can legally appear as a
// call argument in this version: literals, a plain local (via
// body.local_types), std::move/std::make_unique, a nested call's own
// (resolved) return type, and the common unary/binary operators.
// Returns nullopt for anything it can't determine -- notably Member/
// Subscript chains, since movecheck has no access to Program's struct/
// class field-type info here, only Body's per-local types (the same
// scope limitation named_type_name/resolve_callee_signature already
// accept elsewhere). A nullopt argument type makes every candidate
// overload's corresponding parameter fail to match (see
// argument_matches_parameter) -- conservatively rejecting the call with
// a clear diagnostic rather than silently guessing an overload.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::IntegerLiteral: return Type{.kind = TypeKind::Named, .name = "int"};
        case ExprKind::BoolLiteral: return Type{.kind = TypeKind::Named, .name = "bool"};
        case ExprKind::CharLiteral: return Type{.kind = TypeKind::Named, .name = "char"};
        case ExprKind::StringLiteral: {
            Type result;
            result.kind = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = "char"});
            result.is_mutable_pointee = false;
            return result;
        }

        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }

        case ExprKind::Move: {
            // std::move doesn't change the static type -- still whatever
            // std::unique_ptr<T> the moved-from local was declared as.
            if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
            auto it = body.local_types.find(expr.lhs->name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }

        case ExprKind::MakeUnique: {
            Type result;
            result.kind = TypeKind::UniquePtr;
            result.pointee = std::make_shared<Type>(expr.type);
            return result;
        }

        case ExprKind::Lambda: {
            // ch05 §5.12: once resolved (movecheck's closure-resolution
            // pass, which runs before check_moves -- see
            // monomorphize_generics), `expr.name` holds the synthesized
            // closure class's own name; its type is exactly that class,
            // by value (matching MakeUnique's identical shape just
            // above: a fresh, concretely-typed value, not a reference).
            if (expr.name.empty()) return std::nullopt;
            return Type{.kind = TypeKind::Named, .name = expr.name};
        }

        case ExprKind::Unary:
            switch (expr.unary_op) {
                case UnaryOp::Not: return Type{.kind = TypeKind::Named, .name = "bool"};
                case UnaryOp::Neg: return infer_expr_type(*expr.lhs, body, signatures);
                case UnaryOp::AddressOf: {
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    if (!operand) return std::nullopt;
                    Type result;
                    result.kind = TypeKind::Pointer;
                    result.pointee = std::make_shared<Type>(std::move(*operand));
                    // `&expr` always yields a mutable T* (ch05 §5.7) --
                    // whether the place itself is read-only-reachable is
                    // a separate check (is_read_only_reachable), not part
                    // of `&expr`'s own static type.
                    result.is_mutable_pointee = true;
                    return result;
                }
                case UnaryOp::Deref: {
                    std::optional<Type> operand = infer_expr_type(*expr.lhs, body, signatures);
                    if (!operand || (operand->kind != TypeKind::UniquePtr && operand->kind != TypeKind::Pointer)) {
                        return std::nullopt;
                    }
                    return *operand->pointee;
                }
            }
            return std::nullopt;

        case ExprKind::Binary:
            switch (expr.binary_op) {
                case BinaryOp::Add:
                case BinaryOp::Sub:
                case BinaryOp::Mul:
                case BinaryOp::Div:
                case BinaryOp::Assign:
                    return infer_expr_type(*expr.lhs, body, signatures);
                case BinaryOp::Eq:
                case BinaryOp::Ne:
                case BinaryOp::Lt:
                case BinaryOp::Gt:
                case BinaryOp::Le:
                case BinaryOp::Ge:
                case BinaryOp::And:
                case BinaryOp::Or:
                    return Type{.kind = TypeKind::Named, .name = "bool"};
            }
            return std::nullopt;

        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            return sig == nullptr ? std::nullopt : std::optional<Type>(sig->return_type);
        }

        case ExprKind::Member:
        case ExprKind::Subscript:
            // See this function's own doc comment: no Program access
            // here to resolve a struct/class field's or an array/span's
            // element type.
            return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name) {
    auto it = state.find(name);
    return it == state.end() ? LocalState::Bottom : it->second;
}

// Resolves a `base.field` Member expression's own declared field type --
// `base` must be a plain Identifier naming a struct/class-typed local or
// parameter (covers `this.field`, ch05 §5.12's rewritten captured-name
// access, as well as an ordinary `obj.field`); anything else (a nested
// `a.b.c`, `arr[i].field`, ...) returns nullopt, left unsupported for now
// -- see DataflowState::class_field_types' own comment for why this
// lookup is possible at all despite movecheck's otherwise Body-only
// (no Program access) architecture.
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                              const DataflowState& state) {
    if (member_expr.kind != ExprKind::Member || member_expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
    if (state.class_field_types == nullptr) return std::nullopt;
    auto base_it = body.local_types.find(member_expr.lhs->name);
    if (base_it == body.local_types.end()) return std::nullopt;
    const Type& base_type = base_it->second;
    const std::string& type_name = (base_type.kind == TypeKind::Reference ? *base_type.pointee : base_type).name;
    auto class_it = state.class_field_types->find(type_name);
    if (class_it == state.class_field_types->end()) return std::nullopt;
    auto field_it = class_it->second.find(member_expr.name);
    if (field_it == class_it->second.end()) return std::nullopt;
    return field_it->second;
}

// Validates that `operand` (a plain Identifier, e.g. `p`, or a `base.field`
// Member, e.g. `this.p` -- ch05 §5.12's rewritten captured-name access)
// currently names/resolves to a readable pointer-like value that
// `*p`/`p->x` (UnaryOp::Deref) is licensed to dereference: always for a
// std::unique_ptr (this is proven-safe by the move/borrow checker itself,
// no unsafe {} needed), or, only while `state.unsafe_depth > 0` (ch01
// §1.3/ch02/ch05.5), for a raw pointer `T*`. Shared by apply_deref
// (reading through `*p`) so it applies the exact same checks. A `base.field`
// Member operand has no independent move/borrow-state of its own to check
// (movecheck tracks move/borrow state per plain local, not per struct/
// class field -- there is no way to move *out of* a field in this version
// at all, matching the documented pre-existing gap), so it is implicitly
// always considered "Initialized, unborrowed" -- only its *type* (and, for
// a raw pointer, the enclosing unsafe context) is checked.
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body) {
    std::string describe = operand.kind == ExprKind::Member ? operand.lhs->name + "." + operand.name : operand.name;
    std::optional<Type> resolved =
        operand.kind == ExprKind::Member ? resolve_member_field_type(operand, body, state) : [&]() -> std::optional<Type> {
            auto it = body.local_types.find(operand.name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }();
    bool is_uptr = resolved.has_value() && is_unique_ptr(*resolved);
    bool is_raw_ptr = resolved.has_value() && resolved->kind == TypeKind::Pointer;
    if (!is_uptr && !is_raw_ptr) {
        throw DataflowError("cannot dereference ('*') '" + describe +
                             "': only std::unique_ptr or a raw pointer (inside unsafe {}) is supported",
            state.current_loc);
    }
    if (is_raw_ptr && state.unsafe_depth == 0) {
        throw DataflowError("cannot dereference raw pointer '" + describe +
                             "': requires 'unsafe { }' (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    if (operand.kind == ExprKind::Member) return; // no per-field move/borrow state -- see this function's own comment
    LocalState current = lookup(state.locals, operand.name);
    if (current != LocalState::Initialized) {
        throw DataflowError(describe_bad_state(operand.name, current),
            state.current_loc);
    }
}

// Handles a `*p` (UnaryOp::Deref) expression used as a plain read (not
// as a borrow source -- see resolve_borrow_source_root's own Deref case
// for that). Reading *through* a unique_ptr this way does *not* require
// std::move -- unlike reading the unique_ptr identifier `p` directly,
// which always does (spec ch05.1) -- since dereferencing reads the
// pointee's value without disturbing p's own ownership, exactly like
// reading a struct field doesn't move the struct that owns it. A raw
// pointer has no ownership/move state to disturb in the first place.
void apply_deref(const Expr& expr, const DataflowState& state, const Body& body, bool report_errors) {
    bool is_plain_identifier = expr.lhs->kind == ExprKind::Identifier;
    // ch05 §5.12: `*this.p`/`*p` where `p` is an init-captured
    // std::unique_ptr, rewritten to a `this.p` Member access by the
    // closure's own field-access rewrite (rewrite_captured_identifiers_
    // as_field_access) -- see validate_deref_operand's own comment for
    // why a Member operand has no separate move/borrow state to check
    // beyond its type.
    bool is_member_of_identifier =
        expr.lhs->kind == ExprKind::Member && expr.lhs->lhs->kind == ExprKind::Identifier;
    if (!is_plain_identifier && !is_member_of_identifier) {
        if (report_errors) {
            throw DataflowError("dereference ('*') currently only supports a plain local std::unique_ptr or "
                                 "raw pointer variable, or a captured field of one ('this.field') (not a "
                                 "subscript or other expression)",
                state.current_loc);
        }
        return;
    }
    if (!report_errors) return; // purely diagnostic: doesn't move p or change any tracked state
    validate_deref_operand(*expr.lhs, state, body);
    if (!is_plain_identifier) return; // no separate borrow-tracking key for a field -- see the comment above
    const std::string& name = expr.lhs->name;
    auto borrow_it = state.borrows.find(name);
    if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
        // `expr.lhs->loc` (the identifier `name` itself), not
        // `state.current_loc` (the enclosing `*`/Deref's own position,
        // one token earlier) -- both checks above are about `name`
        // specifically, so pointing at it directly is more precise.
        throw DataflowError("cannot use '" + name + "' while it is mutably borrowed",
            expr.lhs->loc);
    }
}

// Resolves `name` to the root place its borrow-tracking should apply to.
// If `name` is itself a currently-bound reference, `ref_targets` already
// stores *its* fully-resolved root directly (every entry is written
// pre-flattened, see apply_reference_binding), so a single lookup -- not
// a manual walk -- is enough to follow a chain of reference-to-reference
// bindings (`const int& s = r;` where `r` is itself `int& r = a;`) back
// to the one real place (`a`) that must be checked/recorded for
// exclusivity. Falls back to `name` itself for an ordinary place or a
// reference *parameter* (opaque from inside this function -- there's no
// caller-side place to resolve to, so its own name is treated as its own
// root; see the Call/apply_reference_argument handling for how the
// caller-side place is checked instead, at each call site).
std::string resolve_root_place(const std::string& name, const DataflowState& state) {
    auto it = state.ref_targets.find(name);
    return it == state.ref_targets.end() ? name : it->second;
}

// Releases the borrow (if any) that reference-typed local `name` holds
// against its root, and forgets that `name` is a currently-bound
// reference. A no-op if `name` isn't (or is no longer) tracked in
// `ref_targets`, so it's safe to call speculatively.
//
// Called from two places (see check_function): as soon as the liveness
// analysis says `name` is no longer live (right after its last use --
// the NLL upgrade from spec ch05.3), and as a fallback at `name`'s
// lexical ScopeExit, for the unusual case of a reference that's never
// read after being bound at all (liveness alone would have released it
// immediately after its BindReference, before ScopeExit is even
// reached). Whichever fires first does the actual work; the other is
// then a harmless no-op, since both leave the exact same state.
void release_reference_borrow(const std::string& name, DataflowState& state, const Body& body) {
    auto ref_it = state.ref_targets.find(name);
    if (ref_it == state.ref_targets.end()) return;
    auto borrow_it = state.borrows.find(ref_it->second);
    if (borrow_it != state.borrows.end()) {
        if (body.local_types.at(name).is_mutable_ref) {
            borrow_it->second.mutable_borrow = false;
        } else if (borrow_it->second.shared_count > 0) {
            borrow_it->second.shared_count--;
        }
        if (!borrow_it->second.mutable_borrow && borrow_it->second.shared_count == 0) {
            state.borrows.erase(borrow_it);
        }
    }
    state.ref_targets.erase(ref_it);
}

std::vector<size_t> successors(const Terminator& term) {
    switch (term.kind) {
        case TerminatorKind::Goto: return {term.target};
        case TerminatorKind::Branch: return {term.true_target, term.false_target};
        case TerminatorKind::Return:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
        default: return {};
    }
}

using LiveSet = std::unordered_set<std::string>;

// Collects the name of every currently-declared *reference-or-span*-typed
// local mentioned anywhere in `expr` (recursively) into `out`. Used by
// the liveness analysis below to find where a reference/span is "used"
// (in the sense of needing its current borrow to stay valid), without
// having to duplicate apply_expr's exact per-case dataflow semantics:
// this walk is deliberately a plain, generic tree traversal,
// over-inclusive rather than clever. A spurious extra "use" only makes a
// borrow's computed live range *longer* than strictly necessary (a
// missed early-release opportunity, but still sound); missing a real one
// would instead be an actual bug (releasing a borrow while it's still
// genuinely needed) -- which is exactly why std::span must be included
// here alongside Reference: without it, a span's borrow would look dead
// (and be released) immediately after its own BindReference, since
// nothing would ever record it as "live", regardless of how long it's
// actually used for afterward.
void collect_reference_uses(const Expr* expr, const Body& body, LiveSet& out) {
    if (expr == nullptr) return;
    switch (expr->kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
            return;
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr->name);
            if (it != body.local_types.end() && (is_reference(it->second) || is_span(it->second))) {
                out.insert(expr->name);
            }
            return;
        }
        case ExprKind::Binary:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Unary:
        case ExprKind::Move:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Call:
        case ExprKind::MakeUnique:
            for (const auto& arg : expr->args) {
                collect_reference_uses(arg.get(), body, out);
            }
            return;
        case ExprKind::Member:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Subscript:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Lambda:
            // ch05 §5.12: a plain (non-init) capture reads whatever
            // local already exists under that name in the enclosing
            // scope -- if that local is itself reference/span-typed,
            // this is a genuine "use" of it, exactly like an ordinary
            // Identifier reference (mirrors the Identifier case above).
            // An init-capture's own expression is walked normally
            // instead (it may itself reference an existing reference-
            // typed local, e.g. `[r = some_ref]`).
            for (const LambdaCapture& capture : expr->lambda_captures) {
                if (capture.init) {
                    collect_reference_uses(capture.init.get(), body, out);
                    continue;
                }
                auto it = body.local_types.find(capture.name);
                if (it != body.local_types.end() && (is_reference(it->second) || is_span(it->second))) {
                    out.insert(capture.name);
                }
            }
            return;
    }
}

// The reference-or-span-typed local that `stmt` freshly brings into
// existence, if any -- only a BindReference does (neither a reference
// nor a span is rebound in this version, so this is also the one and
// only point before which `stmt.local`'s liveness must not extend
// backward; see compute_reference_liveness). Purely keyed off the MIR
// statement kind here, not the local's own type, since mir.cppm already
// emits BindReference for both.
std::optional<std::string> reference_def(const MirStatement& stmt) {
    if (stmt.kind == MirStatementKind::BindReference) return stmt.local;
    return std::nullopt;
}

LiveSet reference_uses(const MirStatement& stmt, const Body& body) {
    LiveSet uses;
    switch (stmt.kind) {
        case MirStatementKind::BindReference:
        case MirStatementKind::Eval:
            collect_reference_uses(stmt.expr, body, uses);
            return uses;
        case MirStatementKind::Assign: {
            collect_reference_uses(stmt.expr, body, uses);
            auto type_it = body.local_types.find(stmt.local);
            if (type_it != body.local_types.end() && is_reference(type_it->second)) {
                // A write-through (`r = expr;` where `r` is itself a
                // reference -- see apply_reference_write_through) reads
                // r's own stored address to know where to write, even
                // though it never reads *through* it.
                uses.insert(stmt.local);
            }
            return uses;
        }
        case MirStatementKind::Declare:
        case MirStatementKind::Drop:
        case MirStatementKind::ScopeExit:
        case MirStatementKind::UnsafeEnter:
        case MirStatementKind::UnsafeExit:
            return uses;
    }
    return uses;
}

LiveSet reference_uses(const Terminator& term, const Body& body) {
    LiveSet uses;
    switch (term.kind) {
        case TerminatorKind::Branch:
            collect_reference_uses(term.condition, body, uses);
            return uses;
        case TerminatorKind::Return:
            collect_reference_uses(term.return_value, body, uses);
            return uses;
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return uses;
    }
    return uses;
}

// Computes, for every statement in `body`, the set of reference-typed
// locals that are *live* (may still be used on some path forward from
// here) immediately after that statement executes -- the backward dual
// of the forward move/borrow dataflow in check_function, and what makes
// this milestone's borrow release NLL-style (spec ch05.3) rather than
// only lexically-scoped (pre-NLL Rust's original, more conservative
// behavior, which is all M4 had).
//
// Standard backward liveness equations, solved to a fixed point over
// the CFG (a single backward pass isn't enough whenever the CFG has a
// loop -- see the `while` case below):
//   live-out(block) = union of live-in(successor) for every successor
//   live-in(block)  = (live-out(block) - defs(block)) + uses(block)
// Verified by hand for a reference declared *and* used entirely inside
// a `while` body: its own BindReference's `defs` kill reverses the
// `uses` gen from later in the *same* iteration before the walk ever
// reaches the block's own entry, so it never appears live going into
// the loop from the back edge (i.e. as if demanded by a *previous*
// iteration) -- exactly as it shouldn't.
//
// `live_after[b][i]` is the live-out set immediately after statement i
// of block b (i.e. live-in to statement i+1, or to the terminator if i
// is the last statement) -- exactly the set check_function needs right
// after applying statement i to decide whether a currently-tracked
// reference has just become dead and should have its borrow released.
std::vector<std::vector<LiveSet>> compute_reference_liveness(const Body& body,
                                                              const std::vector<std::vector<size_t>>& preds) {
    size_t n = body.blocks.size();
    std::vector<LiveSet> block_live_in(n);

    auto block_live_out = [&](size_t b) {
        LiveSet live;
        for (size_t succ : successors(body.blocks[b].terminator)) {
            live.insert(block_live_in[succ].begin(), block_live_in[succ].end());
        }
        return live;
    };

    std::deque<size_t> worklist;
    std::vector<bool> queued(n, false);
    for (size_t i = 0; i < n; i++) {
        worklist.push_back(i);
        queued[i] = true;
    }

    while (!worklist.empty()) {
        size_t b = worklist.front();
        worklist.pop_front();
        queued[b] = false;

        LiveSet live = block_live_out(b);
        const BasicBlock& block = body.blocks[b];
        for (const std::string& use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        for (auto it = block.statements.rbegin(); it != block.statements.rend(); ++it) {
            if (std::optional<std::string> def = reference_def(*it)) live.erase(*def);
            for (const std::string& use : reference_uses(*it, body)) live.insert(use);
        }

        if (live != block_live_in[b]) {
            block_live_in[b] = std::move(live);
            for (size_t p : preds[b]) {
                if (!queued[p]) {
                    worklist.push_back(p);
                    queued[p] = true;
                }
            }
        }
    }

    // Fixed point reached (`block_live_in` is now stable): replay every
    // block once more, this time also recording the live-out-after-each-
    // statement snapshot the forward pass needs.
    std::vector<std::vector<LiveSet>> live_after(n);
    for (size_t b = 0; b < n; b++) {
        const BasicBlock& block = body.blocks[b];
        LiveSet live = block_live_out(b);
        for (const std::string& use : reference_uses(block.terminator, body)) {
            live.insert(use);
        }
        live_after[b].resize(block.statements.size());
        for (size_t i = block.statements.size(); i-- > 0;) {
            live_after[b][i] = live;
            if (std::optional<std::string> def = reference_def(block.statements[i])) live.erase(*def);
            for (const std::string& use : reference_uses(block.statements[i], body)) live.insert(use);
        }
    }
    return live_after;
}

// After executing statement index `i` of a block (whose precomputed
// live-out set is `live_after_stmt`), releases the borrow of every
// currently-tracked reference that's no longer live -- i.e. whose last
// use was this statement or earlier. Collects names to release first
// rather than erasing while iterating `state.ref_targets` directly.
void release_dead_references(DataflowState& state, const Body& body, const LiveSet& live_after_stmt) {
    std::vector<std::string> dead;
    for (const auto& [name, root] : state.ref_targets) {
        if (!live_after_stmt.contains(name)) dead.push_back(name);
    }
    for (const std::string& name : dead) {
        release_reference_borrow(name, state, body);
    }
}

void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors);

// Checks every argument of a Call expression against its callee's
// signature (if known), exactly the same way regardless of context --
// shared by apply_expr's own Call case (a call used as a plain
// statement or value sub-expression) and resolve_borrow_source_root's
// Call case below (a call to a reference-returning function used
// itself as a further reference-binding source).
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                           bool report_errors);

// Resolves the root place that `expr` would be borrowing from if used as
// a reference-binding (`T& r = expr;`) or reference-argument (`f(expr)`
// where the parameter is a reference) source. Supports a plain local
// variable, a chain of `.field`/`[index]` projections off one --
// root-resolved to the *outermost* variable in the chain (see the
// whole-root conservatism note below) -- or a call to a function that
// itself returns a reference, resolved transitively through its own
// elided parameter (spec ch05.3), so a chain of reference-returning
// calls is followed all the way back to a real place.
//
// v0.1 deliberately does *not* do field-sensitive aliasing: borrowing
// `a.x` and `a.y` are both recorded against the *same* root `a`, so they
// conflict with each other exactly as if both borrowed the whole of `a`,
// even though the two fields never actually overlap in memory (spec
// ch05.2). This mirrors how Rust itself treats a dynamically-indexed
// array/slice element (`arr[i]`/`arr[j]` conflict there too, absent an
// explicit split API scpp doesn't have yet) and applies it uniformly to
// struct fields as well, for simplicity. Two workarounds exist for a
// genuinely-disjoint-fields use case: pass each field as its own,
// separate call argument (each such borrow begins and ends within its
// own call -- see apply_reference_argument below -- so sequential calls
// never overlap), or keep the two named reference locals' own live
// ranges (shortened by the liveness analysis below) from overlapping.
[[nodiscard]] std::string resolve_borrow_source_root(const Expr& expr, DataflowState& state, const Body& body,
                                                       const Signatures& signatures, bool report_errors) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            const std::string& bound_name = expr.name;
            if (report_errors) {
                auto type_it = body.local_types.find(bound_name);
                if (type_it != body.local_types.end() && is_unique_ptr(type_it->second)) {
                    throw DataflowError("cannot borrow std::unique_ptr variable '" + bound_name +
                                         "' in this version",
                        state.current_loc);
                }
                LocalState current = lookup(state.locals, bound_name);
                if (current != LocalState::Initialized) {
                    throw DataflowError(describe_bad_state(bound_name, current),
                        state.current_loc);
                }
            }
            return resolve_root_place(bound_name, state);
        }

        case ExprKind::Member:
            // Whole-root conservative (see above): a field projection
            // resolves to the same root as its own base.
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Subscript:
            // The index is a genuine value-producing sub-expression (it
            // could itself read/move/call), so it's checked exactly like
            // any other read; the array base contributes the (whole-)
            // root, same as Member above.
            apply_expr(*expr.rhs, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Unary: {
            // `*p`/`p->x` (a std::unique_ptr local always, or -- only
            // inside unsafe {} -- a raw pointer local; see
            // validate_deref_operand): the root is `p` itself, *not* p's
            // pointee, so that moving or reassigning `p` while a
            // reference into `*p` is alive is rejected by the exact same
            // borrow-conflict checks that already guard every other root
            // (apply_statement's Assign case, and the Move case's own
            // borrow check above) -- freeing or reassigning p's
            // allocation out from under a live reference would otherwise
            // be a use-after-free.
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    throw DataflowError("a reference can currently only borrow a plain local variable, a "
                                         "field of one ('a.b'), an array element of one ('arr[i]'), a "
                                         "dereferenced std::unique_ptr/raw-pointer local ('*p'/'p->x'), or "
                                         "the result of a call to a reference-returning function -- not an "
                                         "arbitrary expression",
                        state.current_loc);
                }
                return "";
            }
            const std::string& name = expr.lhs->name;
            if (report_errors) validate_deref_operand(*expr.lhs, state, body);
            return name;
        }

        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            bool returns_reference = sig != nullptr && sig->elided_param_index.has_value();
            if (!returns_reference) {
                if (report_errors) {
                    throw DataflowError("cannot borrow the result of calling '" + expr.name +
                                         "': it doesn't return a reference with an inferrable lifetime (spec "
                                         "ch05.3)",
                        state.current_loc);
                }
                // Still check the arguments themselves so a genuinely
                // invalid call (wrong callee, bad arguments) is still
                // reported through the ordinary path once report_errors
                // is true; harmless to also run silently here.
                check_call_arguments(expr, state, body, signatures, report_errors);
                return "";
            }
            check_call_arguments(expr, state, body, signatures, report_errors);
            size_t elided_index = *sig->elided_param_index;
            // The elided parameter's *own* argument is what the call's
            // result transitively borrows from -- resolve it the exact
            // same way as any other borrow source (recursively, so a
            // chain of reference-returning calls is followed all the
            // way back to a real place).
            return resolve_borrow_source_root(*expr.args[elided_index], state, body, signatures, report_errors);
        }

        default:
            if (report_errors) {
                throw DataflowError("a reference can currently only borrow a plain local variable, a field of "
                                     "one ('a.b'), an array element of one ('arr[i]'), a dereferenced "
                                     "std::unique_ptr/raw-pointer local ('*p'/'p->x'), or the result of a call "
                                     "to a reference-returning function -- not an arbitrary expression",
                    state.current_loc);
            }
            return "";
    }
}

// Determines whether `expr` (a borrow-source place -- the same shape
// resolve_borrow_source_root accepts) is only reachable *read-only*,
// i.e. whether obtaining a *mutable* `T&`/`T*` from it must be rejected.
// This is the "projection chain's const-reachability" resolve_borrow_
// source_root's own callers need but that function alone doesn't answer
// (it only resolves *which root* to check for borrow conflicts, not
// whether the path to it crossed a read-only step) -- used to reject
// binding a `T&` (apply_reference_binding) or passing a `T&` call
// argument (apply_reference_argument) through a `const T&`/`std::span
// <const T>`/`const T*` anywhere along the chain, and to decide whether
// `&expr` (ch05 §5.7) may produce a mutable `T*` or only a `const T*`.
// Once a chain crosses a read-only step, everything beyond it stays
// read-only: a struct field/array element can never itself be a
// reference or span (ch04.1), so there's no way for a later `.field`/
// `[index]` step to "regain" mutability -- only a chain that never
// crosses one at all is mutable.
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures) {
    switch (expr.kind) {
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr.name);
            if (it == body.local_types.end()) return false; // unknown name: left to codegen's own check
            if (is_reference(it->second) || is_span(it->second)) {
                return !it->second.is_mutable_ref;
            }
            return false; // an owned local (or a by-value parameter) is fully mutable to its owner
        }

        case ExprKind::Member:
        case ExprKind::Subscript:
            return is_read_only_reachable(*expr.lhs, body, signatures);

        case ExprKind::Unary: {
            // `*p`/`p->x`: read-only iff `p` (necessarily a plain
            // Identifier -- see resolve_borrow_source_root's own Unary
            // case) is itself a `const T*` (is_mutable_pointee == false).
            // A std::unique_ptr pointee has no const-qualification
            // concept in this version (it can never be a struct field,
            // and there is no `const std::unique_ptr<T>&` parameter form
            // yet), so it's always mutable through this path.
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) return false;
            auto it = body.local_types.find(expr.lhs->name);
            if (it == body.local_types.end() || it->second.kind != TypeKind::Pointer) return false;
            return !it->second.is_mutable_pointee;
        }

        case ExprKind::Call: {
            // The call's *own* declared return type is authoritative --
            // it doesn't matter whether the elided argument behind it was
            // itself mutable or read-only-reachable: a signature that
            // promises `const T&` back hands back a read-only view
            // regardless (exactly like a plain `const T&`-typed
            // Identifier above), and a signature promising `T&` could
            // only have been called successfully with a mutable-
            // reachable argument in the first place (apply_reference_
            // argument already enforces that at the call site).
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig == nullptr) return false;
            return !sig->return_type.is_mutable_ref;
        }

        default:
            return false;
    }
}

// Handles `&expr` (UnaryOp::AddressOf, ch05 §5.7) used as a plain value.
// Reuses resolve_borrow_source_root to resolve/validate `expr`'s root --
// exactly the same structural resolution (and the same nested side
// effects, e.g. a Subscript index's own apply_expr walk) a `T&`/
// `const T&` binding already goes through (apply_reference_binding) --
// but, unlike that function, registers no lasting borrow afterward: the
// produced `T*` is never move/borrow-tracked (ch05.2 is unchanged by
// this addition), so there's nothing left to later release, and an
// ordinary `T&`/`const T&` borrow of the same place immediately
// afterward is unaffected. Checked only at this instant, conservatively
// (the resulting pointer's eventual use -- read or write -- can't be
// known here): the root must have no existing borrow at all, shared or
// mutable -- the same exclusivity a *new* `T&` binding would require,
// rejected the same way taking a second one would be.
void apply_address_of(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                       bool report_errors) {
    std::string root = resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
    if (!report_errors || root.empty()) return;
    auto borrow_it = state.borrows.find(root);
    if (borrow_it != state.borrows.end() &&
        (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
        throw DataflowError("cannot take the address of '" + root + "': it is already borrowed",
            state.current_loc);
    }
}

// Checks (and, on success, has no lasting effect on `state`) a reference
// function-call argument: a transient borrow that begins right before
// the call and ends right after it returns, since v0.1 never lets a
// reference escape a call (returning one is rejected by codegen's
// declare_function) -- so unlike a local reference variable's borrow
// (released at its own ScopeExit), there's nothing to record past this
// single Call, only a conflict check against whatever is *already*
// borrowed. `in_call_borrows` is a scratch map shared by every reference
// argument of the *same* call (see the Call case below): it catches
// aliasing *within* one argument list (e.g. `f(x, x)` where both
// parameters are references, or one `&mut` and one `&`), which
// `state.borrows` alone can't, since none of these transient borrows are
// ever written back into `state`.
void apply_reference_argument(const Expr& arg, const Type& param_type, DataflowState& state,
                               BorrowMap& in_call_borrows, const Body& body, const Signatures& signatures,
                               bool report_errors) {
    // resolve_borrow_source_root may have real (move-tracking) side
    // effects on `state` via nested apply_expr calls (e.g. a subscript
    // index) that must apply on *every* pass, not just the reporting
    // one -- unlike the rest of this function, which is purely
    // diagnostic (a call argument's borrow never outlives the call, so
    // there's nothing else here for a later statement's fixed-point
    // computation to observe).
    std::string root = resolve_borrow_source_root(arg, state, body, signatures, report_errors);
    if (!report_errors) return;

    bool is_mutable = param_type.is_mutable_ref;

    // Passing an *already-bound* local reference variable directly (`f(r)`
    // where `r` is itself `T& r = ...;`/`const T& r = ...;`) is a
    // reborrow, not a fresh independent borrow: `r` already holds the one
    // live access to `root` (nothing else can coexist with it -- any
    // other attempt to borrow `root` while `r` is alive is already
    // rejected by apply_reference_binding/this same function's
    // persistent-conflict check below), so temporarily re-lending that
    // same access to a callee can't create a new conflict. Only the
    // mutability has to be checked: a shared (`const T&`) reference can't
    // satisfy a `T&` parameter (that would manufacture a mutable alias
    // out of a shared one), but a mutable reference may always be lent
    // out as either mutable or shared.
    bool is_reborrow_of_live_reference = arg.kind == ExprKind::Identifier && state.ref_targets.contains(arg.name);
    if (is_reborrow_of_live_reference) {
        bool source_is_mutable = body.local_types.at(arg.name).is_mutable_ref;
        if (is_mutable && !source_is_mutable) {
            throw DataflowError("cannot pass '" + arg.name + "' by mutable reference: it is itself only a "
                                                              "shared (const) reference",
                state.current_loc);
        }
    } else {
        // The general case: `arg` isn't itself a directly-named, locally-
        // bound reference (that narrower case is handled above), so it
        // may instead be a `.field`/`[index]` projection or a plain
        // *parameter* (never entered into `ref_targets`, since a
        // parameter is never processed through BindReference -- see
        // apply_reference_binding) whose own declared type is `const T&`/
        // `std::span<const T>`, or a chain that dereferences a `const T*`
        // -- any of which must likewise reject manufacturing a mutable
        // reference out of a read-only one (spec ch05 §5.7's "projection
        // chain's const-reachability").
        if (is_mutable && is_read_only_reachable(arg, body, signatures)) {
            throw DataflowError("cannot pass '" + root + "' by mutable reference: it is only reachable "
                                                          "through a read-only (const) reference",
                state.current_loc);
        }
        auto persistent_it = state.borrows.find(root);
        bool persistent_conflict =
            persistent_it != state.borrows.end() &&
            (is_mutable ? (persistent_it->second.mutable_borrow || persistent_it->second.shared_count > 0)
                        : persistent_it->second.mutable_borrow);
        if (persistent_conflict) {
            throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                                 "reference: it is already borrowed",
                state.current_loc);
        }
    }

    auto in_call_it = in_call_borrows.find(root);
    bool in_call_conflict =
        in_call_it != in_call_borrows.end() &&
        (is_mutable ? (in_call_it->second.mutable_borrow || in_call_it->second.shared_count > 0)
                    : in_call_it->second.mutable_borrow);
    if (in_call_conflict) {
        throw DataflowError("cannot pass '" + root + "' by " + std::string(is_mutable ? "mutable " : "") +
                             "reference more than once in the same call",
            state.current_loc);
    }

    BorrowState& borrow = in_call_borrows[root];
    if (is_mutable) {
        borrow.mutable_borrow = true;
    } else {
        borrow.shared_count++;
    }
}

// Checks every argument of a Call expression against its callee's
// signature (if known), exactly the same way regardless of context --
// shared by apply_expr's own Call case (a call used as a plain
// statement or value sub-expression) and resolve_borrow_source_root's
// Call case below (a call to a reference-returning function used
// itself as a further reference-binding source). Also the single place
// (reached from every Call site) that enforces ch02/ch05.5's "calling an
// `extern \"C\"` function requires `unsafe {}`" rule (ch01 §1.3):
// rejected only when the callee is *known* (an unresolved/unknown callee
// name is left to codegen's own "call to unknown function" check, same
// treatment as elsewhere in this file) and is an `extern "C"`
// declaration, and the call site itself isn't currently inside an
// `unsafe { }` block (state.unsafe_depth > 0 -- see check_function's
// entry_state setup and DataflowState::unsafe_depth). Every other
// callee -- an ordinary scpp function or a bare `extern` (ch11 §11.6)
// declaration alike -- is checked by default (ch01) and needs no
// `unsafe {}` to call at all. print_int/print_bool and other
// codegen-only builtins are never in `signatures` at all, so they're
// always callable regardless of context, same as they already bypass
// every other signature-based check in this file.
void check_call_arguments(const Expr& expr, DataflowState& state, const Body& body, const Signatures& signatures,
                           bool report_errors) {
    CalleeSignature callee = resolve_callee_signature(expr, body);
    auto name_it = signatures.find(callee.key);
    const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
    // ch05 §5.10: a name that exists but has no overload whose parameters
    // match this call's actual arguments is a hard error (an explicit
    // cast/a genuinely matching overload is required) -- distinct from
    // "the name doesn't exist at all", which this function has never
    // rejected itself (left to codegen's own "call to unknown function"
    // check; preserved here unchanged).
    if (report_errors && name_it != signatures.end() && sig == nullptr) {
        throw DataflowError("no overload of '" + expr.name +
                             "' matches these argument types (spec ch05.10 -- overload resolution is exact "
                             "type match only; an explicit cast may be required)",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->is_extern_c && state.unsafe_depth == 0) {
        throw DataflowError("cannot call 'extern \"C\"' function '" + expr.name +
                             "' outside 'unsafe { }': no scpp compiler ever sees its real implementation to "
                             "check it (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    // Scratch borrow-map shared by every reference argument of *this*
    // call only (see apply_reference_argument) -- never merged into
    // `state`, since none of these transient borrows outlive the call.
    BorrowMap in_call_borrows;
    for (size_t i = 0; i < expr.args.size(); i++) {
        const Expr& arg = *expr.args[i];
        size_t param_index = i + callee.param_offset;
        bool param_is_reference =
            sig != nullptr && param_index < sig->param_types.size() && is_reference(sig->param_types[param_index]);
        bool param_is_rvalue_reference = param_is_reference && sig->param_types[param_index].is_rvalue_ref;
        if (param_is_rvalue_reference) {
            // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- an ownership-
            // transfer argument, not a borrow: needs a genuine rvalue
            // (see produces_rvalue_of_type), never apply_reference_
            // argument's place-borrow bookkeeping. Still walked via
            // apply_expr (exactly like a by-value/unique_ptr argument
            // below) for its own side effects -- e.g. std::move(x)
            // marking x moved-out in `state`.
            if (report_errors &&
                !produces_rvalue_of_type(arg, *sig->param_types[param_index].pointee, body, signatures)) {
                throw DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc);
            }
            apply_expr(arg, /*is_unique_ptr_rvalue_context=*/true, state, body, signatures, report_errors);
        } else if (param_is_reference) {
            apply_reference_argument(arg, sig->param_types[param_index], state, in_call_borrows, body, signatures,
                                      report_errors);
        } else {
            // Parameter types aren't resolved here for anything other
            // than reference detection above; a unique_ptr argument
            // must be `std::move(x)` regardless of position, so
            // std::move is simply allowed in every non-reference
            // argument slot.
            apply_expr(arg, /*is_unique_ptr_rvalue_context=*/true, state, body, signatures, report_errors);

            // `&expr` (ch05 §5.7) passed directly as a call argument --
            // the primary motivating use case (an `extern "C"` out
            // parameter, e.g. `getsockopt(..., &value, &len)`). If the
            // declared parameter wants a *mutable* `T*` but `expr`'s
            // place is only reachable read-only, reject: same
            // const-widens-only-one-way rule as everywhere else in this
            // version (`const T*` never converts to `T*`, so there is no
            // way to legitimately satisfy a mutable-pointee parameter
            // here). Scoped to exactly this direct syntactic shape (not a
            // general type-checker): a raw pointer value that has already
            // passed through some other variable/call has no
            // "reachability" left to check -- only its own already-
            // enforced declared constness matters by then (see
            // assignment_target_is_read_only's Unary case).
            bool param_wants_mutable_pointer =
                sig != nullptr && param_index < sig->param_types.size() &&
                sig->param_types[param_index].kind == TypeKind::Pointer &&
                sig->param_types[param_index].is_mutable_pointee;
            if (report_errors && param_wants_mutable_pointer && arg.kind == ExprKind::Unary &&
                arg.unary_op == UnaryOp::AddressOf && is_read_only_reachable(*arg.lhs, body, signatures)) {
                throw DataflowError("cannot pass '&' of a read-only-reachable place as a mutable 'T*' "
                                    "argument (would need 'const T*', which this parameter doesn't accept)",
                    state.current_loc);
            }
        }
    }
}

// ch05 §5.12: checks every capture of a resolved Lambda literal --
// shared by apply_expr's own Lambda case (a *transient* use: an IIFE, a
// call argument, ... -- the closure literal itself can never outlive
// this statement, so `reference_capture_borrows` is a fresh, local,
// discarded-afterward map there) and apply_statement's class-typed
// Assign case (a closure literal being bound to a *named* `auto`
// variable, ch05 §5.12's only spelling for this -- which genuinely can
// outlive this statement, so that caller passes `state.borrows` itself,
// making any by-reference capture's borrow last for the rest of this
// function, exactly like an ordinary `T& r = x;` binding's own borrow
// would -- a deliberately conservative simplification: released only at
// function end rather than at the closure variable's own precise last
// use, since v0.1 has no liveness analysis for a class-typed local the
// way it already has for a plain reference/span, see
// compute_reference_liveness). Checked the same way passing each
// capture as an argument already would be (reusing existing machinery,
// zero new move/borrow logic beyond this per-capture dispatch):
//  - an init-capture's own expression is evaluated normally (e.g.
//    permitting std::move(p) for a move-only type).
//  - a plain by-value capture is an ordinary read: rejects an un-moved
//    std::unique_ptr, exactly like reading any other Identifier (there
//    is no implicit-copy escape hatch for a move-only type -- use an
//    init-capture with std::move instead, ch05 §5.12's own example).
//  - a by-reference capture is checked exactly like a reference-typed
//    call argument (apply_reference_argument): the closure's own field
//    genuinely borrows it, for as long as `reference_capture_borrows`
//    (see above) says it lasts.
void apply_lambda_captures(const Expr& expr, DataflowState& state, BorrowMap& reference_capture_borrows,
                            const Body& body, const Signatures& signatures, bool report_errors) {
    for (const LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init) {
            apply_expr(*capture.init, /*is_unique_ptr_rvalue_context=*/true, state, body, signatures, report_errors);
            continue;
        }
        if (!capture.by_reference) {
            if (report_errors) {
                auto type_it = body.local_types.find(capture.name);
                if (type_it != body.local_types.end() && is_unique_ptr(type_it->second)) {
                    throw DataflowError(
                        "use of std::unique_ptr variable '" + capture.name +
                            "' requires std::move (copying is not allowed) -- capture it as an "
                            "init-capture instead, e.g. '[" +
                            capture.name + " = std::move(" + capture.name + ")]' (ch05 §5.12)",
                        state.current_loc);
                }
            }
            LocalState current = lookup(state.locals, capture.name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(capture.name, current), state.current_loc);
            }
            continue;
        }
        Expr capture_ident;
        capture_ident.kind = ExprKind::Identifier;
        capture_ident.loc = expr.loc;
        capture_ident.name = capture.name;
        Type ref_type;
        ref_type.kind = TypeKind::Reference;
        ref_type.is_mutable_ref = true; // matches resolve_lambda's own field choice
        apply_reference_argument(capture_ident, ref_type, state, reference_capture_borrows, body, signatures,
                                  report_errors);
    }
}

// Walks `expr`, updating `state` for any std::move / assignment / borrow
// side effects and, when `report_errors` is true, throwing DataflowError
// on an unsafe read. `is_unique_ptr_rvalue_context` is true exactly where
// a bare `std::move(x)` is allowed to appear: a var-decl initializer, an
// assignment RHS, a return value, or a call argument.
//
// This function is run twice per program point: once during the
// worklist's fixed-point iteration (report_errors=false, just to compute
// stable per-block states) and once more in the final reporting pass
// (report_errors=true). Both runs must apply the *same* state mutations so
// the two phases stay consistent.
void apply_expr(const Expr& expr, bool is_unique_ptr_rvalue_context, DataflowState& state, const Body& body,
                 const Signatures& signatures, bool report_errors) {
    // Refreshed on every call (including each recursive call for a child
    // sub-expression) so that, by the time *this* node's own checks run
    // (whether before any recursive call, like the Identifier case just
    // below, or after one, like Member's access-control check further
    // down), `state.current_loc` reliably points at `expr` itself rather
    // than whatever child was most recently visited -- see
    // DataflowState::current_loc.
    state.current_loc = expr.loc;
    switch (expr.kind) {
        case ExprKind::IntegerLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
            return;

        case ExprKind::Identifier: {
            if (!report_errors) return;
            auto type_it = body.local_types.find(expr.name);
            if (type_it == body.local_types.end()) return; // unknown name: left to codegen's own check
            if (is_unique_ptr(type_it->second)) {
                throw DataflowError("use of std::unique_ptr variable '" + expr.name +
                                     "' requires std::move (copying is not allowed)",
                    state.current_loc);
            }
            LocalState current = lookup(state.locals, expr.name);
            if (current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(expr.name, current),
                    state.current_loc);
            }
            // A direct read is rejected if `expr.name` is currently
            // serving as the root of an active *mutable* borrow --
            // alias-XOR-mutability means only that borrow's own
            // reference may access it while live. Note `state.borrows`
            // is naturally keyed only by roots: a reference that itself
            // borrows something (e.g. `r` in `int& r = a;`) is never a
            // key here (its borrow is recorded, pre-flattened, against
            // `a`; see resolve_root_place), so reading `r` by its own
            // name to go *through* the very borrow it holds is never
            // blocked by this check -- only reading the aliased root
            // directly (`a`, or an opaque reference parameter that some
            // other local reference borrows from) is.
            auto borrow_it = state.borrows.find(expr.name);
            if (borrow_it != state.borrows.end() && borrow_it->second.mutable_borrow) {
                throw DataflowError("cannot use '" + expr.name + "' while it is mutably borrowed",
                    state.current_loc);
            }
            return;
        }

        case ExprKind::Move: {
            if (expr.lhs->kind != ExprKind::Identifier) {
                if (report_errors) {
                    throw DataflowError("std::move currently only supports a plain local variable "
                                         "(not a member, subscript, or other expression)",
                        state.current_loc);
                }
                return;
            }
            const std::string& name = expr.lhs->name;
            auto type_it = body.local_types.find(name);
            if (type_it == body.local_types.end() || !is_unique_ptr(type_it->second)) {
                if (report_errors) {
                    throw DataflowError("std::move is only supported for std::unique_ptr variables in this "
                                         "version; '" +
                                         name + "' is not one",
                        state.current_loc);
                }
                return;
            }
            LocalState current = lookup(state.locals, name);
            if (report_errors && current != LocalState::Initialized) {
                throw DataflowError(describe_bad_state(name, current),
                    state.current_loc);
            }
            if (report_errors) {
                auto borrow_it = state.borrows.find(name);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    throw DataflowError("cannot move '" + name + "' while it is borrowed",
                        state.current_loc);
                }
            }
            state.locals[name] = LocalState::MovedOut;
            if (report_errors && !is_unique_ptr_rvalue_context) {
                throw DataflowError("std::move(" + name + ") must be used to initialize or assign into a "
                                                            "std::unique_ptr",
                    state.current_loc);
            }
            return;
        }

        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Deref) {
                apply_deref(expr, state, body, report_errors);
                return;
            }
            if (expr.unary_op == UnaryOp::AddressOf) {
                apply_address_of(expr, state, body, signatures, report_errors);
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_unique_ptr = false;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    target_is_unique_ptr = it != body.local_types.end() && is_unique_ptr(it->second);
                }
                apply_expr(*expr.rhs, target_is_unique_ptr, state, body, signatures, report_errors);
                if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*expr.rhs, body, signatures)) {
                    throw DataflowError("assigning to a std::unique_ptr variable requires std::move or "
                                        "std::make_unique (copying is not allowed)",
                        state.current_loc);
                }
                if (expr.lhs->kind == ExprKind::Identifier) {
                    // The assignment target is never a "read": whatever
                    // its previous state, assigning any value returns it
                    // to Initialized (spec ch05.1).
                    state.locals[expr.lhs->name] = LocalState::Initialized;
                } else {
                    // e.g. `p.x = 1;` or `arr[i] = 1;`: the base
                    // object/index are evaluated (as addresses / an
                    // index value), not read as "the assignment target",
                    // so still worth walking for nested reads.
                    apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
                    if (report_errors) {
                        if (assignment_target_is_read_only(*expr.lhs, body, signatures)) {
                            throw DataflowError("cannot assign to this place: it is reached through a "
                                                 "read-only (const) reference",
                                state.current_loc);
                        }
                        if (std::optional<std::string> root = direct_write_root(*expr.lhs, body)) {
                            auto borrow_it = state.borrows.find(*root);
                            if (borrow_it != state.borrows.end() &&
                                (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                                throw DataflowError("cannot assign to this place: '" + *root +
                                                     "' is currently borrowed",
                                    state.current_loc);
                            }
                        }
                    }
                }
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Call:
            check_call_arguments(expr, state, body, signatures, report_errors);
            return;

        case ExprKind::Member: {
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            // ch04 §4.2: a member variable is always private-by-
            // construction (parse_class_def rejects `public` on one
            // outright), so external code can only ever reach it through
            // a method call -- never directly, the way it still can for
            // a struct field (never access-controlled, ch04 §4.1).
            // Scoped to a plain Identifier base (`this`, or an ordinary
            // local/parameter) for now -- movecheck doesn't otherwise
            // infer the type of an arbitrary nested expression, so a
            // deeper chain (`a.b.field` where `a.b` is itself
            // class-typed) isn't covered by this check yet, a known,
            // narrow scope limitation.
            if (report_errors && expr.lhs->kind == ExprKind::Identifier && state.class_names != nullptr) {
                auto type_it = body.local_types.find(expr.lhs->name);
                if (type_it != body.local_types.end()) {
                    std::string class_name = named_type_name(type_it->second);
                    if (!class_name.empty() && state.class_names->contains(class_name) &&
                        class_name != state.current_class) {
                        throw DataflowError("cannot access private member '" + expr.name + "' of class '" +
                                             class_name + "' from outside its own methods (ch04 §4.2)",
                            state.current_loc);
                    }
                }
            }
            return;
        }

        case ExprKind::Subscript:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::MakeUnique:
            // std::make_unique<T>(args...) itself never reads a
            // unique_ptr (it *produces* one); its constructor arguments
            // are ordinary values, checked as plain reads.
            for (const auto& arg : expr.args) {
                apply_expr(*arg, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            }
            return;

        case ExprKind::Lambda: {
            // ch05 §5.12: a resolved lambda literal constructs a fresh
            // instance of its synthesized closure class, binding each
            // capture. When the literal is used *transiently* (an
            // IIFE, a call argument, ...) it can never outlive this
            // statement (scpp has no way to name/store a closure value
            // beyond this one -- unless it's the direct initializer of
            // an `auto` variable, see apply_statement's own Assign
            // case, which calls apply_lambda_captures directly with
            // `state.borrows` itself instead of a throwaway map), so a
            // fresh, local, discarded-afterward BorrowMap here is sound
            // -- see apply_lambda_captures' own comment for the shared
            // per-capture logic.
            BorrowMap capture_borrows;
            apply_lambda_captures(expr, state, capture_borrows, body, signatures, report_errors);
            return;
        }
    }
}

// Handles a `BindReference` MIR statement -- `T& r = place;` /
// `const T& r = place;` (emitted only by a Reference-typed VarDecl), or
// `std::span<T> s = arr;` / `std::span<const T> s = arr;` (emitted only
// by a Span-typed VarDecl) -- see mir.cppm. Checks the borrowed place is
// currently readable and not already borrowed in a conflicting way
// (ch05.2's alias-XOR-mutability), then records the new borrow -- against
// the place's *root* (see resolve_borrow_source_root), not necessarily
// `place` itself, so a chain of reference-to-reference bindings (and a
// `.field`/`[index]` projection off a plain place) is tracked precisely.
void apply_reference_binding(const MirStatement& stmt, DataflowState& state, const Body& body,
                              const Signatures& signatures, bool report_errors) {
    if (stmt.expr == nullptr) {
        // No initializer (`int& r;` / `std::span<int> s;`): illegal,
        // since unlike every other scpp type, neither a reference nor a
        // span has a zero/default state to fall back to -- real C++ has
        // no such thing as a null or later-bound reference either, and
        // v0.1 conservatively requires the same discipline of span (see
        // apply_statement's Assign case for why it isn't rebindable
        // either).
        if (report_errors) {
            const char* kind_name = is_span(stmt.type) ? "span" : "reference";
            throw DataflowError(std::string(kind_name) + " '" + stmt.local +
                                 "' must be initialized (bound to a variable) at declaration",
                state.current_loc);
        }
        state.locals[stmt.local] = LocalState::Initialized;
        return;
    }

    std::string root = resolve_borrow_source_root(*stmt.expr, state, body, signatures, report_errors);
    if (root.empty()) {
        // Only reachable when report_errors=false and the source
        // expression's shape isn't (yet) a supported borrow source --
        // resolve_borrow_source_root would have thrown had report_errors
        // been true, so this whole program is already doomed to be
        // rejected by the upcoming reporting pass; just leave `stmt.local`
        // itself readable so this (discarded) silent fixed-point
        // iteration has *some* defined state to continue from.
        state.locals[stmt.local] = LocalState::Initialized;
        return;
    }

    bool is_mutable = stmt.type.is_mutable_ref;

    // Reject manufacturing a mutable `T&`/`std::span<T>` out of a place
    // that's only reachable read-only (e.g. `int& r = p.x;`/
    // `std::span<int> s = p.arr;` where `p` is `const Foo&`) -- spec
    // ch05 §5.7's "projection chain's const-reachability" check, shared
    // with apply_reference_argument's identical guard for a call
    // argument. A `const T&`/`std::span<const T>` binding is always fine
    // regardless (read-only never needs to widen).
    if (report_errors && is_mutable && is_read_only_reachable(*stmt.expr, body, signatures)) {
        const char* kind_name = is_span(stmt.type) ? "span" : "reference";
        throw DataflowError(std::string("cannot bind a mutable ") + kind_name + " '" + stmt.local +
                             "': its source is only reachable through a read-only (const) reference",
            state.current_loc);
    }

    BorrowState& borrow = state.borrows[root];
    if (report_errors) {
        if (is_mutable && (borrow.mutable_borrow || borrow.shared_count > 0)) {
            throw DataflowError("cannot mutably borrow '" + root + "': it is already borrowed",
                state.current_loc);
        }
        if (!is_mutable && borrow.mutable_borrow) {
            throw DataflowError("cannot borrow '" + root + "': it is already mutably borrowed",
                state.current_loc);
        }
    }
    if (is_mutable) {
        borrow.mutable_borrow = true;
    } else {
        borrow.shared_count++;
    }
    state.ref_targets[stmt.local] = root;
    state.locals[stmt.local] = LocalState::Initialized;
}

// Handles a plain `r = expr;` MIR Assign statement where `r` was
// *previously* bound as a reference (its VarDecl went through
// BindReference, not this path -- see mir.cppm). Real C++ references
// can't be rebound, so this always means "write through `r` to its
// current referent", not "rebind r": rejected outright for a `const T&`
// (read-only), otherwise just an ordinary write with no borrow-conflict
// check needed here, since `r` holding a live mutable borrow *is* the
// license to write through it (see the Identifier-case comment in
// apply_expr for the symmetric read-side reasoning).
void apply_reference_write_through(const MirStatement& stmt, DataflowState& state, const Body& body,
                                    const Signatures& signatures, bool report_errors) {
    const Type& ref_type = body.local_types.at(stmt.local);
    if (report_errors) {
        if (!ref_type.is_mutable_ref) {
            throw DataflowError("cannot assign through '" + stmt.local +
                                 "': it is a read-only (const) reference",
                state.current_loc);
        }
        LocalState current = lookup(state.locals, stmt.local);
        if (current != LocalState::Initialized) {
            throw DataflowError(describe_bad_state(stmt.local, current),
                state.current_loc);
        }
    }
    apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
}

void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors) {
    // See apply_expr's identical opening comment -- same reasoning, one
    // level up (statement rather than expression granularity).
    state.current_loc = stmt.loc;
    switch (stmt.kind) {
        case MirStatementKind::Declare:
            // scpp has no "uninitialized" state (see the LocalState
            // comment above): a bare declaration always zero-initializes,
            // so it's always Initialized from this point on.
            state.locals[stmt.local] = LocalState::Initialized;
            return;

        case MirStatementKind::BindReference:
            apply_reference_binding(stmt, state, body, signatures, report_errors);
            return;

        case MirStatementKind::Assign: {
            auto type_it = body.local_types.find(stmt.local);
            if (type_it != body.local_types.end() && is_reference(type_it->second)) {
                apply_reference_write_through(stmt, state, body, signatures, report_errors);
                return;
            }
            if (type_it != body.local_types.end() && is_span(type_it->second)) {
                // Unlike real C++ (where std::span is an ordinary,
                // freely-reassignable value), v0.1 conservatively treats
                // it exactly like a reference: bound once at
                // declaration, never rebound (see mir.cppm's
                // BindReference comment) -- lifting that is a follow-up,
                // not a soundness requirement.
                if (report_errors) {
                    throw DataflowError("std::span '" + stmt.local +
                                         "' cannot be reassigned after initialization in this version",
                        state.current_loc);
                }
                return;
            }
            if (type_it != body.local_types.end() && state.class_names != nullptr &&
                type_it->second.kind == TypeKind::Named && state.class_names->contains(type_it->second.name)) {
                // ch04 §4.2: unlike a plain `struct` (an ordinary,
                // freely-reassignable trivial value), a class-typed local
                // is conservatively bound once at construction and never
                // reassigned in this version -- this *is* a soundness
                // necessity, not just a temporary restriction: without a
                // real copy constructor/assignment operator (out of
                // scope for v0.1 -- ch04's own "full class checking
                // rules" note), a plain bitwise reassignment would copy
                // whatever resource-owning fields the class has (e.g. a
                // raw handle its destructor later frees), and both the
                // old and new bindings' destructors would then
                // independently try to release the *same* resource at
                // their respective scope exits -- a double-free/use-
                // after-free. Lifting this needs real copy semantics
                // designed first, not just permission to reassign.
                //
                // This MIR-level Assign statement, though, is *also* how
                // a `VarDecl`'s own `= expr` initializer lowers (see
                // mir.cppm's VarDecl case -- there is no separate
                // "construct with an initial value" MIR node) -- the
                // *only* spelling `auto name = expr;` (ch05 §5.12, the
                // sole way to name a closure's own otherwise-unspellable
                // type) can ever take. A genuine first initialization
                // must therefore still be allowed here: distinguished
                // from a later reassignment by whether `stmt.local` has
                // *any* prior entry in `state.locals` at all (a bare
                // `ClassName c;` -- the only other way to declare a
                // class-typed local -- always marks it Initialized
                // immediately, see the Declare case above; a plain
                // `auto f = expr;` has no such preceding Declare, so its
                // own Assign is always this variable's first-ever
                // appearance).
                if (report_errors && state.locals.contains(stmt.local)) {
                    throw DataflowError("class '" + type_it->second.name + "'-typed variable '" + stmt.local +
                                         "' cannot be reassigned after construction in this version (no copy "
                                         "semantics are defined yet -- see ch04 §4.2)",
                        state.current_loc);
                }
                if (stmt.expr->kind == ExprKind::Lambda) {
                    // ch05 §5.12: unlike a *transient* lambda literal
                    // (apply_expr's own Lambda case -- an IIFE, a call
                    // argument, ...), one bound to a named `auto`
                    // variable genuinely can outlive this statement, so
                    // any by-reference capture's borrow must land
                    // directly in `state.borrows` (persisting for the
                    // rest of this function -- see
                    // apply_lambda_captures' own comment) rather than a
                    // throwaway map that apply_expr's generic Lambda
                    // handling would otherwise use.
                    apply_lambda_captures(*stmt.expr, state, state.borrows, body, signatures, report_errors);
                } else {
                    apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures,
                               report_errors);
                }
                state.locals[stmt.local] = LocalState::Initialized;
                return;
            }

            bool target_is_unique_ptr = type_it != body.local_types.end() && is_unique_ptr(type_it->second);
            apply_expr(*stmt.expr, target_is_unique_ptr, state, body, signatures, report_errors);
            if (report_errors && target_is_unique_ptr && !produces_unique_ptr_rvalue(*stmt.expr, body, signatures)) {
                throw DataflowError("variable '" + stmt.local +
                                    "' of type std::unique_ptr must be initialized via std::move or "
                                    "std::make_unique (copying a unique_ptr is not allowed)",
                    state.current_loc);
            }

            // `T* p = &expr;` (ch05 §5.7): if `p`'s declared type wants a
            // *mutable* T* but `expr`'s place is only reachable
            // read-only, reject -- the same const-widens-only-one-way
            // rule as check_call_arguments's identical guard for a call
            // argument (`const T*` never converts to `T*` in this
            // version, so there is no way to legitimately satisfy a
            // mutable-pointee declaration here). Scoped to exactly this
            // direct syntactic shape, not a general type-checker -- see
            // check_call_arguments's identical comment for why.
            if (report_errors && type_it != body.local_types.end() && type_it->second.kind == TypeKind::Pointer &&
                type_it->second.is_mutable_pointee && stmt.expr->kind == ExprKind::Unary &&
                stmt.expr->unary_op == UnaryOp::AddressOf && is_read_only_reachable(*stmt.expr->lhs, body, signatures)) {
                throw DataflowError("cannot assign '&' of a read-only-reachable place to '" + stmt.local +
                                    "' (a mutable 'T*'): would need 'const T*', which '" + stmt.local +
                                    "' isn't declared as",
                    state.current_loc);
            }

            if (report_errors) {
                auto borrow_it = state.borrows.find(stmt.local);
                if (borrow_it != state.borrows.end() &&
                    (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                    throw DataflowError("cannot assign to '" + stmt.local + "' while it is borrowed",
                        state.current_loc);
                }
            }
            state.locals[stmt.local] = LocalState::Initialized;
            return;
        }

        case MirStatementKind::Eval:
            apply_expr(*stmt.expr, /*is_unique_ptr_rvalue_context=*/false, state, body, signatures, report_errors);
            return;

        case MirStatementKind::Drop:
            // Purely a codegen-facing marker (no-op until heap-allocated
            // owning types exist); no dataflow state effect here.
            return;

        case MirStatementKind::ScopeExit: {
            // Releases `stmt.local`'s borrow, in the (unusual) case it's
            // a reference that was never read after being bound, so the
            // liveness-driven release in check_function never got a
            // chance to fire for it (a no-op here otherwise -- see
            // release_reference_borrow). Moving or dropping a *borrowed*
            // root while a borrow is still live is not separately
            // checked here: it can't happen in v0.1, since a reference
            // can only ever be bound to a *plain* place (or a `.field`/
            // `[index]` projection off one) declared no later than (i.e.
            // in the same or an enclosing scope of) the reference
            // itself, so the borrow is always released -- at the latest
            // at the reference's own ScopeExit -- before the root's own
            // ScopeExit (if any) is ever reached -- and the only
            // *movable* type (unique_ptr) can't be referenced at all yet
            // (see codegen's validate_reference_pointee), so "move a
            // borrowed place" can't arise either.
            release_reference_borrow(stmt.local, state, body);
            // `stmt.local` just went out of lexical scope: forget its
            // tracked state entirely. Erasing is equivalent to setting
            // it to Bottom (lookup() treats a missing key as Bottom) and
            // keeps the map from growing with entries the rest of the
            // analysis no longer cares about.
            state.locals.erase(stmt.local);
            return;
        }

        case MirStatementKind::UnsafeEnter:
            state.unsafe_depth++;
            return;

        case MirStatementKind::UnsafeExit:
            state.unsafe_depth--;
            return;
    }
}

void check_terminator(const Terminator& term, DataflowState& state, const Function& fn, const Body& body,
                       const Signatures& signatures) {
    // See apply_expr's identical opening comment.
    state.current_loc = term.loc;
    switch (term.kind) {
        case TerminatorKind::Branch:
            apply_expr(*term.condition, false, state, body, signatures, /*report_errors=*/true);
            return;
        case TerminatorKind::Return: {
            if (term.return_value == nullptr) return;
            if (is_reference(fn.return_type)) {
                // The elision rule (spec ch05.3) was already validated
                // structurally once for the whole program (see
                // resolve_elided_param_index, called from check_moves)
                // -- recomputing it here (a pure function of `fn` alone,
                // no Signatures lookup needed: `fn` is already the one
                // specific, already-resolved overload being checked, not
                // a name that could denote several) is guaranteed to
                // have a value. What's left to check per return
                // statement is the actual *dangling* risk: does this
                // specific returned expression really borrow (directly,
                // or transitively through a chain of locals/calls) from
                // that one parameter, or does it borrow something else --
                // most importantly, a purely local place, which would
                // dangle the instant this function returns and its stack
                // frame is popped.
                const std::string& elided_param_name = fn.params[*resolve_elided_param_index(fn)].name;
                std::string returned_root =
                    resolve_borrow_source_root(*term.return_value, state, body, signatures, /*report_errors=*/true);
                if (returned_root != elided_param_name) {
                    throw DataflowError(
                        "function '" + fn.name + "' returns a reference derived from '" + returned_root +
                        "', not from its sole reference parameter '" + elided_param_name +
                        "'; scpp v0.1 can only prove a returned reference doesn't dangle when it borrows "
                        "(directly or transitively) from that parameter (spec ch05.3)",
                        state.current_loc);
                }
                return;
            }
            bool return_is_unique_ptr = is_unique_ptr(fn.return_type);
            apply_expr(*term.return_value, return_is_unique_ptr, state, body, signatures, /*report_errors=*/true);
            if (return_is_unique_ptr && !produces_unique_ptr_rvalue(*term.return_value, body, signatures)) {
                throw DataflowError(
                    "returning a std::unique_ptr requires std::move or std::make_unique (copying is not allowed)",
                    state.current_loc);
            }
            return;
        }
        case TerminatorKind::Goto:
        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            return;
    }
}

// Runs the worklist algorithm (see spec ch07/M3) to a fixed point over
// `body`'s CFG, computing a stable per-block entry ("IN") state for the
// definite-initialization/move/borrow lattice above, then makes one more
// pass reporting any unsafe use found using those now-stable states.
// Splitting into these two phases avoids both false positives (from
// not-yet-stable intermediate states) and duplicate diagnostics (a block
// can be visited many times during fixed-point iteration).
void check_function(const Function& fn, const Signatures& signatures, const std::unordered_set<std::string>& class_names,
                     const ClassFieldTypes& class_field_types) {
    Body body = build_mir(fn);

    size_t n = body.blocks.size();

    std::vector<std::vector<size_t>> preds(n);
    for (size_t i = 0; i < n; i++) {
        for (size_t succ : successors(body.blocks[i].terminator)) {
            preds[succ].push_back(i);
        }
    }

    // Precomputed once, up front: which reference locals are still live
    // (may be used again) immediately after each statement -- see
    // compute_reference_liveness. Consulted after every apply_statement
    // call below (in *both* passes) to release a reference's borrow as
    // soon as its last use has happened, rather than waiting for its
    // ScopeExit -- the NLL upgrade from spec ch05.3.
    std::vector<std::vector<LiveSet>> live_after = compute_reference_liveness(body, preds);

    DataflowState entry_state;
    // ch01 §1.3: every function is checked by default, unconditionally --
    // there is no per-function way to start already inside an implicit
    // unsafe context (the old "native function" concept is fully
    // retired). Every function's entry_state therefore starts at 0;
    // unsafe_depth only ever increases via an explicit, lexically nested
    // `unsafe { }` block within this same function's own body.
    entry_state.unsafe_depth = 0;
    // ch04 §4.2/ch05 §5.9: `this` is always params[0] when present (see
    // parser's make_this_param) -- a user can never spell a same-named
    // parameter themselves, since `this` is a keyword, not an ordinary
    // identifier token.
    if (!fn.params.empty() && fn.params[0].name == "this") {
        entry_state.current_class = fn.params[0].type.pointee->name;
    }
    entry_state.class_names = &class_names;
    entry_state.class_field_types = &class_field_types;
    for (const Param& param : fn.params) {
        entry_state.locals[param.name] = LocalState::Initialized;
        // ch04 §4.2: like a class-typed local (see the Assign case
        // below), a class-typed *parameter* cannot be passed by value
        // either -- scpp has no copy constructor, so a by-value
        // parameter would bitwise-copy whatever resource-owning fields
        // the class has, and the callee's copy would then run its own
        // destructor on the same underlying resource at scope-exit,
        // independently of the caller's original -- a double-free. Take
        // a `const T&`/`T&` parameter instead (this doesn't apply to
        // `this` itself, which is always Reference-typed already, never
        // bare Named -- see make_this_param).
        if (param.type.kind == TypeKind::Named && class_names.contains(param.type.name)) {
            throw DataflowError("parameter '" + param.name + "' of function '" + fn.name + "' cannot take class '" +
                                 param.type.name +
                                 "' by value (no copy semantics are defined yet -- take 'const " +
                                 param.type.name + "&' or '" + param.type.name + "&' instead, see ch04 §4.2)",
                                 fn.loc);
        }
    }
    // Symmetric to the by-value-parameter rejection above: returning a
    // class by value would require copying it out of the callee's local
    // scope, the exact same unsupported bitwise-copy-of-a-resource-
    // owning-value hazard. A method/function that hands back a class
    // instance must do so via a reference to an already-existing one
    // (e.g. `this`, via the this-elision rule, ch05 §5.3/§5.9) or the
    // caller must construct its own directly -- there is no by-value
    // "return a freshly-built class instance" form in this version.
    if (fn.return_type.kind == TypeKind::Named && class_names.contains(fn.return_type.name)) {
        throw DataflowError("function '" + fn.name + "' cannot return class '" + fn.return_type.name +
                             "' by value (no copy semantics are defined yet -- return '" + fn.return_type.name +
                             "&'/'const " + fn.return_type.name + "&' instead, see ch04 §4.2)",
                             fn.loc);
    }

    std::vector<DataflowState> in_states(n);
    std::vector<DataflowState> out_states(n);
    if (n > 0) in_states[0] = entry_state;

    std::deque<size_t> worklist;
    std::vector<bool> queued(n, false);
    for (size_t i = 0; i < n; i++) {
        worklist.push_back(i);
        queued[i] = true;
    }

    while (!worklist.empty()) {
        size_t b = worklist.front();
        worklist.pop_front();
        queued[b] = false;

        DataflowState new_in;
        if (b == 0) {
            new_in = entry_state;
        } else {
            bool first = true;
            for (size_t p : preds[b]) {
                new_in = first ? out_states[p] : join_states(new_in, out_states[p]);
                first = false;
            }
        }

        DataflowState new_out = new_in;
        for (size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            apply_statement(body.blocks[b].statements[i], new_out, body, signatures, /*report_errors=*/false);
            release_dead_references(new_out, body, live_after[b][i]);
        }

        in_states[b] = new_in;
        bool out_changed = !(new_out == out_states[b]);
        out_states[b] = std::move(new_out);

        if (out_changed) {
            for (size_t succ : successors(body.blocks[b].terminator)) {
                if (!queued[succ]) {
                    worklist.push_back(succ);
                    queued[succ] = true;
                }
            }
        }
    }

    // Fixed point reached: `in_states` is now stable. Walk every block
    // once more, this time actually reporting diagnostics.
    for (size_t b = 0; b < n; b++) {
        DataflowState state = in_states[b];
        for (size_t i = 0; i < body.blocks[b].statements.size(); i++) {
            apply_statement(body.blocks[b].statements[i], state, body, signatures, /*report_errors=*/true);
            release_dead_references(state, body, live_after[b][i]);
        }
        check_terminator(body.blocks[b].terminator, state, fn, body, signatures);
    }
}

// Builds the ch05 §5.10 overload-resolution signature map from every
// Function in `program` -- factored out of check_moves so
// monomorphize_generics (ch05 §5.11, below) can build the same map for
// its own call-site type inference (infer_expr_type/resolve_overload)
// without duplicating this loop. Throws the same "redefinition" /
// "invalid elision" diagnostics check_moves itself always has, just
// possibly surfaced slightly earlier in the pipeline now that
// monomorphization runs before check_moves (see driver.cppm) -- the
// error is exactly as correct either way.
[[nodiscard]] Signatures build_signatures(const Program& program) {
    Signatures signatures;
    for (const Function& fn : program.functions) {
        FunctionSignature sig;
        sig.param_types.reserve(fn.params.size());
        for (const Param& param : fn.params) {
            sig.param_types.push_back(param.type);
        }
        sig.return_type = fn.return_type;
        sig.elided_param_index = resolve_elided_param_index(fn);
        sig.is_extern_c = fn.is_extern_c;
        sig.loc = fn.loc;
        std::vector<FunctionSignature>& overloads = signatures[fn.name];
        for (const FunctionSignature& existing : overloads) {
            bool same_params = existing.param_types.size() == sig.param_types.size();
            for (size_t i = 0; same_params && i < sig.param_types.size(); i++) {
                same_params = types_equal(existing.param_types[i], sig.param_types[i]);
            }
            if (same_params) {
                throw DataflowError("redefinition of '" + fn.name +
                                     "': a previous declaration with an identical parameter list already "
                                     "exists (ch05 §5.10 -- functions can only be overloaded by parameter "
                                     "list, return type alone doesn't count as a difference)",
                    fn.loc);
            }
        }
        overloads.push_back(std::move(sig));
    }
    return signatures;
}

// ch05 §5.11: a deep (recursive) copy of an Expr/Stmt tree -- needed
// only for monomorphization (below), which must inject an independent
// clone of a generic template's body per concrete instantiation (Stmt/
// Expr trees use unique_ptr children with no copy constructor of their
// own, by design -- see Expr/Stmt's own comments in ast.cppm).
ExprPtr clone_expr(const Expr& expr) {
    auto clone = std::make_unique<Expr>();
    clone->kind = expr.kind;
    clone->loc = expr.loc;
    clone->int_value = expr.int_value;
    clone->bool_value = expr.bool_value;
    clone->name = expr.name;
    clone->binary_op = expr.binary_op;
    if (expr.lhs) clone->lhs = clone_expr(*expr.lhs);
    if (expr.rhs) clone->rhs = clone_expr(*expr.rhs);
    clone->unary_op = expr.unary_op;
    clone->args.reserve(expr.args.size());
    for (const ExprPtr& arg : expr.args) clone->args.push_back(clone_expr(*arg));
    clone->type = expr.type;
    return clone;
}

StmtPtr clone_stmt(const Stmt& stmt) {
    auto clone = std::make_unique<Stmt>();
    clone->kind = stmt.kind;
    clone->loc = stmt.loc;
    clone->type = stmt.type;
    clone->var_name = stmt.var_name;
    if (stmt.init) clone->init = clone_expr(*stmt.init);
    clone->has_ctor_args = stmt.has_ctor_args;
    clone->ctor_args.reserve(stmt.ctor_args.size());
    for (const ExprPtr& arg : stmt.ctor_args) clone->ctor_args.push_back(clone_expr(*arg));
    if (stmt.expr) clone->expr = clone_expr(*stmt.expr);
    if (stmt.condition) clone->condition = clone_expr(*stmt.condition);
    if (stmt.then_branch) clone->then_branch = clone_stmt(*stmt.then_branch);
    if (stmt.else_branch) clone->else_branch = clone_stmt(*stmt.else_branch);
    clone->statements.reserve(stmt.statements.size());
    for (const StmtPtr& s : stmt.statements) clone->statements.push_back(clone_stmt(*s));
    clone->is_unsafe = stmt.is_unsafe;
    return clone;
}

// ch05 §5.11: whether `type` (a concrete, ordinary type -- never a
// witness class) structurally satisfies `concept_def`: for every
// requirement, the class named by `type` must have a real method
// matching the requirement's own shape exactly -- same synthesized name
// (`ClassName_methodName`, see ClassDef's own comment), same argument
// types (exact match, ch05 §5.10 -- no implicit conversions), and (only
// when the requirement itself constrains it) an identical return type.
// A simple requirement (no return-type constraint) only requires the
// method to exist with matching arguments -- its own return type is
// unconstrained, so any return type qualifies.
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def, const Program& program) {
    if (type.kind != TypeKind::Named) return false;
    for (const ConceptRequirement& req : concept_def.requirements) {
        std::string method_name = type.name + "_" + req.method_name;
        bool found = false;
        for (const Function& fn : program.functions) {
            if (fn.name != method_name || fn.params.empty()) continue;
            if (fn.params.size() != req.arg_types.size() + 1) continue;
            bool args_match = true;
            for (size_t i = 0; args_match && i < req.arg_types.size(); i++) {
                args_match = types_equal(fn.params[i + 1].type, req.arg_types[i]);
            }
            if (!args_match) continue;
            if (req.has_return_constraint && !types_equal(fn.return_type, req.return_type)) continue;
            found = true;
            break;
        }
        if (!found) return false;
    }
    return true;
}

// A short, deterministic, LLVM-identifier-safe encoding of `type` for a
// monomorphized clone's own name -- deliberately duplicated from
// codegen's own (private, inaccessible from here) mangle_type rather
// than shared across modules, same existing precedent as this file's
// own independently-duplicated types_equal.
[[nodiscard]] std::string mangle_type_for_clone_name(const Type& type) {
    switch (type.kind) {
        case TypeKind::Named: return type.name;
        case TypeKind::Pointer:
            return mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
        case TypeKind::UniquePtr: return mangle_type_for_clone_name(*type.pointee) + "_uptr";
        case TypeKind::Reference:
            return mangle_type_for_clone_name(*type.pointee) +
                   (type.is_rvalue_ref ? "_rref" : (type.is_mutable_ref ? "_ref" : "_cref"));
        case TypeKind::Span: return mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_ref ? "_span" : "_cspan");
        case TypeKind::Array:
            return mangle_type_for_clone_name(*type.element) + "_arr" + std::to_string(type.array_size);
    }
    return "?";
}

// ch05 §5.12: collects every VarDecl's own name inside `stmt`
// (recursively, ignoring lexical scope -- same "whole-body, flat"
// pragmatism as mir.cppm's own Body::local_types) into `out` -- used to
// exclude a lambda's own locally-declared variables from blanket-
// capture free-variable resolution (they're the lambda's own locals,
// never a capture).
void collect_locally_declared_names(const Stmt& stmt, std::unordered_set<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            out.insert(stmt.var_name);
            return;
        case StmtKind::If:
            collect_locally_declared_names(*stmt.then_branch, out);
            if (stmt.else_branch) collect_locally_declared_names(*stmt.else_branch, out);
            return;
        case StmtKind::While:
            collect_locally_declared_names(*stmt.then_branch, out);
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) collect_locally_declared_names(*s, out);
            return;
        default:
            return;
    }
}

// Forward declarations: collect_free_identifiers's Expr/Stmt overloads
// are mutually recursive (an If/While/Block statement recurses into its
// own sub-expressions/sub-statements; a Lambda expression recurses into
// its own body statement).
void collect_free_identifiers(const Expr& expr, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out);
void collect_free_identifiers(const Stmt& stmt, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out);

// ch05 §5.12: collects every free Identifier reference inside `expr`
// (skipping any name in `excluded` -- a lambda's own params/locals,
// already-explicit captures, known function names, known type names)
// into `out`, for a blanket `[=]`/`[&]` capture's own free-variable
// resolution. A Call's own callee name and a Member's own field name
// are never variable references (skipped entirely); their receiver/
// base sub-expressions are still walked normally. A nested lambda's own
// body is conservatively walked too (v1 simplification: over-
// approximates rather than precisely tracking what a nested lambda
// already captures itself -- harmless, since capturing an unused name
// is safe, just not maximally minimal; see this codebase's general
// "pragmatic over exhaustive" style elsewhere).
void collect_free_identifiers(const Expr& expr, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out) {
    if (expr.kind == ExprKind::Identifier) {
        if (!excluded.contains(expr.name)) out.insert(expr.name);
        return;
    }
    if (expr.lhs) collect_free_identifiers(*expr.lhs, excluded, out);
    if (expr.rhs) collect_free_identifiers(*expr.rhs, excluded, out);
    for (const ExprPtr& arg : expr.args) collect_free_identifiers(*arg, excluded, out);
    if (expr.kind == ExprKind::Lambda && expr.lambda_body) {
        collect_free_identifiers(*expr.lambda_body, excluded, out);
    }
}

void collect_free_identifiers(const Stmt& stmt, const std::unordered_set<std::string>& excluded,
                                std::unordered_set<std::string>& out) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) collect_free_identifiers(*stmt.init, excluded, out);
            for (const ExprPtr& arg : stmt.ctor_args) collect_free_identifiers(*arg, excluded, out);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) collect_free_identifiers(*stmt.expr, excluded, out);
            return;
        case StmtKind::If:
            collect_free_identifiers(*stmt.condition, excluded, out);
            collect_free_identifiers(*stmt.then_branch, excluded, out);
            if (stmt.else_branch) collect_free_identifiers(*stmt.else_branch, excluded, out);
            return;
        case StmtKind::While:
            collect_free_identifiers(*stmt.condition, excluded, out);
            collect_free_identifiers(*stmt.then_branch, excluded, out);
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) collect_free_identifiers(*s, excluded, out);
            return;
    }
}

// ch05 §5.12: rewrites every bare Identifier reference to a captured
// name inside `expr` into an explicit `this.name` Member access.
// Necessary because a lambda's body, as originally written, refers to
// what were then ordinary enclosing-scope locals by their bare names --
// but scpp requires *explicit* `this.field` for a class's own fields
// (there is no implicit-field-lookup fallback the way real C++ allows
// a bare `field` inside a method body, verified empirically: this
// codebase's own movecheck rejects a bare field reference with "use of
// undeclared variable"). Once the lambda's body becomes the synthesized
// closure class's own "call" method, each captured name is a *field*,
// not a local, so every such reference must be rewritten this way. The
// lambda's own parameters and any of its own locally-declared variables
// are left as ordinary bare identifiers -- only names in
// `captured_names` are ever rewritten. A nested lambda's own body is
// conservatively rewritten too (matching collect_free_identifiers'
// identical reasoning above); this codebase does not attempt to prove a
// nested lambda's own parameter/local never shadows a captured name
// from this outer scope -- an accepted v1 limitation (not demonstrated
// by any documented example, and real C++ closure-capture-chain nesting
// is itself a well-known deep-end topic).
void rewrite_captured_identifiers_as_field_access(Stmt& stmt, const std::unordered_set<std::string>& captured_names);

void rewrite_captured_identifiers_as_field_access(Expr& expr, const std::unordered_set<std::string>& captured_names) {
    if (expr.kind == ExprKind::Identifier && captured_names.contains(expr.name)) {
        auto this_ref = std::make_unique<Expr>();
        this_ref->kind = ExprKind::Identifier;
        this_ref->loc = expr.loc;
        this_ref->name = "this";
        expr.kind = ExprKind::Member;
        expr.lhs = std::move(this_ref);
        // expr.name already holds the captured name -- Member's own
        // field-name slot, unchanged.
        return;
    }
    if (expr.lhs) rewrite_captured_identifiers_as_field_access(*expr.lhs, captured_names);
    if (expr.rhs) rewrite_captured_identifiers_as_field_access(*expr.rhs, captured_names);
    for (ExprPtr& arg : expr.args) rewrite_captured_identifiers_as_field_access(*arg, captured_names);
    if (expr.kind == ExprKind::Lambda && expr.lambda_body) {
        rewrite_captured_identifiers_as_field_access(*expr.lambda_body, captured_names);
    }
}

void rewrite_captured_identifiers_as_field_access(Stmt& stmt, const std::unordered_set<std::string>& captured_names) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) rewrite_captured_identifiers_as_field_access(*stmt.init, captured_names);
            for (ExprPtr& arg : stmt.ctor_args) rewrite_captured_identifiers_as_field_access(*arg, captured_names);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) rewrite_captured_identifiers_as_field_access(*stmt.expr, captured_names);
            return;
        case StmtKind::If:
            rewrite_captured_identifiers_as_field_access(*stmt.condition, captured_names);
            rewrite_captured_identifiers_as_field_access(*stmt.then_branch, captured_names);
            if (stmt.else_branch) rewrite_captured_identifiers_as_field_access(*stmt.else_branch, captured_names);
            return;
        case StmtKind::While:
            rewrite_captured_identifiers_as_field_access(*stmt.condition, captured_names);
            rewrite_captured_identifiers_as_field_access(*stmt.then_branch, captured_names);
            return;
        case StmtKind::Block:
            for (StmtPtr& s : stmt.statements) rewrite_captured_identifiers_as_field_access(*s, captured_names);
            return;
    }
}

// ch05 §5.12: "By default a closure's `operator()` is const (a by-value
// capture can't be reassigned inside the body)"; `mutable` opts out.
// Checked directly here -- on the *original*, pre field-access-rewrite
// body, where a captured name is still an ordinary bare Identifier, so
// no field-type information is needed -- rather than via the general
// const-`this`-propagation mechanism (assignment_target_is_read_only),
// which cannot by itself distinguish "writing to a by-value field"
// (should require `mutable`) from "writing *through* a by-reference
// field's own referent" (always allowed, regardless of `mutable` --
// matching real C++, where a reference member's constness is
// independent of its enclosing object's own) -- see resolve_lambda's
// own `this_type.is_mutable_ref` comment for why the "call" method's
// receiver is unconditionally mutable. Only ever called when the
// lambda is *not* `mutable` (see resolve_lambda). A nested lambda's own
// body is deliberately not recursed into: it has its own independent
// capture list, checked when *it* is itself resolved.
void reject_write_to_nonmutable_by_value_capture(const Expr& expr, const std::unordered_set<std::string>& by_value_names) {
    if (expr.kind == ExprKind::Binary && expr.binary_op == BinaryOp::Assign && expr.lhs->kind == ExprKind::Identifier &&
        by_value_names.contains(expr.lhs->name)) {
        throw DataflowError("cannot assign to by-value-captured '" + expr.lhs->name +
                                 "' inside a non-'mutable' lambda (ch05 §5.12 -- a closure's own call operator "
                                 "is 'const' by default; add 'mutable' to opt out)",
            expr.loc);
    }
    if (expr.lhs) reject_write_to_nonmutable_by_value_capture(*expr.lhs, by_value_names);
    if (expr.rhs) reject_write_to_nonmutable_by_value_capture(*expr.rhs, by_value_names);
    for (const ExprPtr& arg : expr.args) reject_write_to_nonmutable_by_value_capture(*arg, by_value_names);
}

void reject_write_to_nonmutable_by_value_capture(const Stmt& stmt, const std::unordered_set<std::string>& by_value_names) {
    switch (stmt.kind) {
        case StmtKind::VarDecl:
            if (stmt.init) reject_write_to_nonmutable_by_value_capture(*stmt.init, by_value_names);
            for (const ExprPtr& arg : stmt.ctor_args) reject_write_to_nonmutable_by_value_capture(*arg, by_value_names);
            return;
        case StmtKind::Return:
        case StmtKind::ExprStmt:
            if (stmt.expr) reject_write_to_nonmutable_by_value_capture(*stmt.expr, by_value_names);
            return;
        case StmtKind::If:
            reject_write_to_nonmutable_by_value_capture(*stmt.condition, by_value_names);
            reject_write_to_nonmutable_by_value_capture(*stmt.then_branch, by_value_names);
            if (stmt.else_branch) reject_write_to_nonmutable_by_value_capture(*stmt.else_branch, by_value_names);
            return;
        case StmtKind::While:
            reject_write_to_nonmutable_by_value_capture(*stmt.condition, by_value_names);
            reject_write_to_nonmutable_by_value_capture(*stmt.then_branch, by_value_names);
            return;
        case StmtKind::Block:
            for (const StmtPtr& s : stmt.statements) reject_write_to_nonmutable_by_value_capture(*s, by_value_names);
            return;
    }
}


// ch05 §5.12: implements call-site monomorphization for every generic
// (concept-constrained) function in a Program -- run once, before
// check_moves (see driver.cppm/monomorphize_generics below), so that by
// the time movecheck's ordinary exact-type-match call-argument checking
// runs, every call site targets an already-concrete, already-
// monomorphized Function (an ordinary function by then, needing zero
// special-casing) rather than the original generic template (whose own
// witness-typed signature would otherwise never structurally match a
// concrete argument). The original generic template itself is left
// completely untouched in `program.functions` -- it's still separately,
// abstractly checked by movecheck's ordinary per-function pass (its
// witness-typed `this`/parameters make that check exactly as
// intraprocedural as any other function's), just never itself reachable
// from a real call site anymore, and excluded from codegen entirely
// (Codegen::generate, keyed off Function::is_generic_template).
//
// Also resolves every closure literal (ch05 §5.12) in the same pass --
// both features need the same thing (concrete per-function type
// information the parser itself never has), so they share this single
// pre-check_moves walk rather than needing two separate passes over
// every function body.
class Monomorphizer {
public:
    explicit Monomorphizer(Program& program) : program_(program) {
        for (const ConceptDef& c : program.concepts) concepts_by_name_[c.name] = &c;
        for (size_t i = 0; i < program.functions.size(); i++) {
            if (program.functions[i].is_generic_template) generic_template_indices_[program.functions[i].name] = i;
        }
        // ch05 §5.12: names a blanket lambda capture must never
        // implicitly bind to -- a known type name (struct/class/
        // concept) or a known free-function name is never itself a
        // capturable *variable*, even though it may appear as a bare
        // Identifier-shaped token inside a requires-expression-like
        // context; excluded up front so collect_free_identifiers'
        // exclusion set (built per-lambda in resolve_lambda) doesn't
        // need to reconstruct these each time.
        for (const StructDef& s : program.structs) known_type_names_.insert(s.name);
        for (const ClassDef& c : program.classes) known_type_names_.insert(c.name);
        for (const ConceptDef& c : program.concepts) known_type_names_.insert(c.name);
        for (const Function& fn : program.functions) known_function_names_.insert(fn.name);
    }

    void run() {
        signatures_ = build_signatures(program_);
        // A snapshot of the function count *before* any clone is
        // injected: new clones/synthesized closure classes are appended
        // to program_.functions/program_.classes as we go (see
        // get_or_create_clone/resolve_lambda) and must never themselves
        // be re-walked by *this* outer loop (they're already fully
        // concrete -- nothing left to monomorphize/resolve at the top
        // level; a synthesized closure's own body is instead walked
        // directly from within resolve_lambda itself, once, right after
        // being synthesized).
        //
        // A generic template's own body *is* walked here too (unlike an
        // earlier version of this pass) -- bare-call-redirect (ch05
        // §5.9/§5.11/§5.12, e.g. `f(x)` for a witness-typed parameter
        // `f`) and lambda-resolution both need to run there just as much
        // as anywhere else, and neither depends on knowing the eventual
        // concrete instantiation. Only the *generic-call-monomorphization*
        // half of walk_expr is suppressed while inside a generic
        // template's own body (see allow_generic_monomorphization
        // below) -- a nested generic-template-calling-another-generic-
        // template call site is left targeting the original,
        // codegen-excluded template, surfacing as a clear "unknown
        // function" error downstream rather than incorrectly attempting
        // to monomorphize against an abstract witness type as if it
        // were concrete.
        size_t original_count = program_.functions.size();
        for (size_t i = 0; i < original_count; i++) {
            if (program_.functions[i].body == nullptr) continue;
            // build_mir's own Body holds raw (const Expr*) pointers into
            // this Function's *own* Stmt/Expr tree (see mir.cppm's
            // Terminator) -- safe to keep using after program_.functions
            // mutates below, since only the *vector's* backing storage
            // (and, incidentally, the Function objects it directly
            // holds) can move; a Function's own body is heap-allocated
            // independently (via StmtPtr/ExprPtr) and never relocates
            // just because the enclosing vector reallocates elsewhere.
            Body body = build_mir(program_.functions[i]);
            bool allow_generic_monomorphization = !program_.functions[i].is_generic_template;
            walk_stmt(*program_.functions[i].body, body, this_type_of(program_.functions[i]),
                      allow_generic_monomorphization);
        }
    }

private:
    Program& program_;
    std::unordered_map<std::string, const ConceptDef*> concepts_by_name_;
    std::unordered_map<std::string, size_t> generic_template_indices_;
    std::unordered_map<std::string, std::string> clone_cache_;
    std::unordered_set<std::string> known_type_names_;
    std::unordered_set<std::string> known_function_names_;
    Signatures signatures_;
    // ch05 §5.12: a monotonically-increasing counter for synthesizing
    // each closure's own unique class name ("__lambda0", "__lambda1",
    // ...) -- a lambda literal has no user-spelled name to reuse (unlike
    // a concept's witness class, which shares the concept's own name),
    // and this codebase has no other source of process-wide uniqueness
    // to draw on.
    int lambda_counter_ = 0;

    // ch04 §4.2/ch05 §5.9: the enclosing function's own `this` parameter
    // type (Named(ClassName)), or nullopt if `fn` isn't a method at all
    // -- used to type a `[this]` lambda capture. `this` is always
    // params[0] when present (parser's make_this_param).
    [[nodiscard]] static std::optional<Type> this_type_of(const Function& fn) {
        if (fn.params.empty() || fn.params[0].name != "this") return std::nullopt;
        return *fn.params[0].type.pointee;
    }

    void walk_stmt(Stmt& stmt, Body& body, const std::optional<Type>& enclosing_this_type,
                   bool allow_generic_monomorphization) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) walk_expr(*stmt.init, body, enclosing_this_type, allow_generic_monomorphization);
                for (ExprPtr& arg : stmt.ctor_args) {
                    walk_expr(*arg, body, enclosing_this_type, allow_generic_monomorphization);
                }
                // ch05 §5.12: `auto name = expr;` -- infer the concrete
                // type from the (by-now-fully-resolved, e.g. a Lambda's
                // own synthesized class) initializer. Must overwrite
                // *both* the AST's own `stmt.type` (so check_moves/
                // codegen's later, fresh `build_mir` call sees a
                // concrete type) and this pass's own `body.local_types`
                // entry in place (so a *later* statement in this same
                // function -- e.g. `f(x)`'s bare-call-redirect just
                // below, or another lambda capturing `f` by reference --
                // resolves this variable's real type too, not the stale
                // "auto" placeholder `build_mir` originally saw before
                // any resolution ran).
                if (stmt.type.kind == TypeKind::Named && stmt.type.name == "auto") {
                    if (!stmt.init) {
                        throw DataflowError("'auto' requires an initializer", stmt.loc);
                    }
                    std::optional<Type> inferred = infer_expr_type(*stmt.init, body, signatures_);
                    if (!inferred.has_value()) {
                        throw DataflowError(
                            "cannot infer 'auto' variable '" + stmt.var_name + "'s type from its initializer",
                            stmt.loc);
                    }
                    stmt.type = *inferred;
                    body.local_types[stmt.var_name] = *inferred;
                }
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) walk_expr(*stmt.expr, body, enclosing_this_type, allow_generic_monomorphization);
                return;
            case StmtKind::If:
                walk_expr(*stmt.condition, body, enclosing_this_type, allow_generic_monomorphization);
                walk_stmt(*stmt.then_branch, body, enclosing_this_type, allow_generic_monomorphization);
                if (stmt.else_branch) {
                    walk_stmt(*stmt.else_branch, body, enclosing_this_type, allow_generic_monomorphization);
                }
                return;
            case StmtKind::While:
                walk_expr(*stmt.condition, body, enclosing_this_type, allow_generic_monomorphization);
                walk_stmt(*stmt.then_branch, body, enclosing_this_type, allow_generic_monomorphization);
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) {
                    walk_stmt(*s, body, enclosing_this_type, allow_generic_monomorphization);
                }
                return;
        }
    }

    void walk_expr(Expr& expr, Body& body, const std::optional<Type>& enclosing_this_type,
                   bool allow_generic_monomorphization) {
        // ch05 §5.12: a Lambda's own sub-tree (captures' init-exprs,
        // params, body) is handled entirely inside resolve_lambda --
        // never via the generic lhs/rhs/args recursion below (which
        // would find them all empty/unused for a Lambda node anyway,
        // since captures/params/body are Lambda's own dedicated fields,
        // not lhs/rhs/args -- see ast.cppm's Expr).
        if (expr.kind == ExprKind::Lambda) {
            resolve_lambda(expr, body, enclosing_this_type);
            return;
        }

        // ch05 §5.9/§5.11/§5.12: a bare (no-receiver) Call whose own
        // name resolves to a *local variable* (not a function) of class
        // type is sugar for calling that class's own "call" method --
        // `f(args)` desugars in place to `f.call(args)`, reusing 100% of
        // the existing method-call machinery with zero further new
        // logic. Shared by an ordinary user-defined callable class (any
        // class with a method literally named "call"), a concept's own
        // witness class (ch05 §5.11's IntConsumer-style direct-
        // invocation requirement, e.g. `f(x)` inside a generic
        // function's own body), and a real closure (ch05 §5.12's
        // `c(args)`). A local variable always shadows an outer function
        // of the same name here, matching ordinary C++ scoping -- and
        // there is no genuine ambiguity in practice, since a generic
        // template's own name (checked further below) is never itself
        // registered as a local.
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr) {
            auto local_it = body.local_types.find(expr.name);
            if (local_it != body.local_types.end()) {
                const Type& local_type = local_it->second;
                const Type& underlying = local_type.kind == TypeKind::Reference ? *local_type.pointee : local_type;
                if (underlying.kind == TypeKind::Named) {
                    auto receiver = std::make_unique<Expr>();
                    receiver->kind = ExprKind::Identifier;
                    receiver->loc = expr.loc;
                    receiver->name = expr.name;
                    expr.lhs = std::move(receiver);
                    expr.name = "call";
                }
            }
        }

        if (expr.lhs) walk_expr(*expr.lhs, body, enclosing_this_type, allow_generic_monomorphization);
        if (expr.rhs) walk_expr(*expr.rhs, body, enclosing_this_type, allow_generic_monomorphization);
        for (ExprPtr& arg : expr.args) walk_expr(*arg, body, enclosing_this_type, allow_generic_monomorphization);

        // Only a bare (no-receiver) Call can ever target a generic
        // template -- generic *methods* are rejected at parse time (see
        // parser.cppm's reject_generic_params), so `expr.lhs != nullptr`
        // (a method-call shape) can never be one. Suppressed entirely
        // while walking a generic template's own body (see run()'s own
        // comment): a nested generic-to-generic call is left targeting
        // the original, codegen-excluded template instead.
        if (!allow_generic_monomorphization) return;
        if (expr.kind != ExprKind::Call || expr.lhs != nullptr) return;
        auto template_it = generic_template_indices_.find(expr.name);
        if (template_it == generic_template_indices_.end()) return;
        const Function& tmpl = program_.functions[template_it->second];

        std::vector<Type> concrete_param_types;
        concrete_param_types.reserve(tmpl.params.size());
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (param.generic_concept.empty()) {
                concrete_param_types.push_back(param.type);
                continue;
            }
            if (i >= expr.args.size()) return; // arg-count mismatch -- leave for codegen's own error
            std::optional<Type> arg_type = infer_expr_type(*expr.args[i], body, signatures_);
            if (!arg_type.has_value()) return;
            // The concept is checked against the argument's *underlying*
            // named type -- e.g. a `const Shape auto&` parameter's
            // argument might itself be a plain `Circle` local (arg_type
            // == Named("Circle")) or an already-bound `const Circle&`
            // reference variable (arg_type == Reference(Circle)) -- both
            // resolve to the same concrete type for concept-satisfaction
            // and substitution purposes.
            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
            auto concept_it = concepts_by_name_.find(param.generic_concept);
            if (concept_it == concepts_by_name_.end()) return;
            if (!type_satisfies_concept(named, *concept_it->second, program_)) {
                throw DataflowError("argument type '" + named.name + "' does not satisfy concept '" +
                                     param.generic_concept + "' required by generic function '" + tmpl.name +
                                     "' (ch05 §5.11 -- every requirement's method must exist with a matching "
                                     "signature)",
                    expr.loc);
            }
            Type substituted = param.type;
            if (substituted.kind == TypeKind::Reference) {
                substituted.pointee = std::make_shared<Type>(named);
            } else {
                substituted = named;
            }
            concrete_param_types.push_back(std::move(substituted));
        }

        expr.name = get_or_create_clone(tmpl, concrete_param_types);
    }

    // ch05 §5.12: resolves a single Lambda expression node in place --
    // performs blanket-capture free-variable analysis if needed,
    // resolves every capture's concrete field type, synthesizes the
    // concrete closure ClassDef + "call" method (injecting both into
    // program_), rewrites the (deep-cloned) body's captured-name
    // references into explicit `this.name` field access, and finally
    // sets `expr.name` to the synthesized class -- the only thing
    // codegen/movecheck need from here on to treat this literal exactly
    // like an ordinary class construction (see codegen's own Lambda
    // case).
    void resolve_lambda(Expr& expr, Body& enclosing_body, const std::optional<Type>& enclosing_this_type) {
        if (expr.lambda_blanket_mode != LambdaCaptureMode::None) {
            std::unordered_set<std::string> excluded;
            for (const Param& p : expr.lambda_params) excluded.insert(p.name);
            for (const LambdaCapture& c : expr.lambda_captures) excluded.insert(c.name);
            if (expr.lambda_body) collect_locally_declared_names(*expr.lambda_body, excluded);
            excluded.insert(known_function_names_.begin(), known_function_names_.end());
            excluded.insert(known_type_names_.begin(), known_type_names_.end());

            std::unordered_set<std::string> free_names;
            if (expr.lambda_body) collect_free_identifiers(*expr.lambda_body, excluded, free_names);
            bool by_reference = expr.lambda_blanket_mode == LambdaCaptureMode::ByReference;
            for (const std::string& name : free_names) {
                // ch05 §5.12's own hard rule: `this` is never implicitly
                // captured by a bare `[=]`/`[&]`, even though it would
                // otherwise look like just another free identifier here
                // -- must be named explicitly (`[this]`/`[=, this]`/
                // `[&, this]`).
                if (name == "this") continue;
                // Not a real local in the enclosing scope -- leave for
                // the usual "use of undeclared variable" error rather
                // than guessing.
                if (!enclosing_body.local_types.contains(name)) continue;
                LambdaCapture capture;
                capture.name = name;
                capture.by_reference = by_reference;
                expr.lambda_captures.push_back(std::move(capture));
            }
        }

        std::vector<Type> field_types;
        field_types.reserve(expr.lambda_captures.size());
        std::unordered_set<std::string> captured_names;
        // ch05 §5.12: every by-*value*-captured name other than `this`
        // (`[*this]`'s own copy semantics are a separate concern --
        // see below) -- used after the loop to reject a direct
        // assignment to one of these inside a non-`mutable` lambda body
        // (reject_write_to_nonmutable_by_value_capture). A by-*reference*
        // capture is deliberately excluded: writing *through* a
        // reference field is always allowed regardless of the closure's
        // own mutability, exactly like real C++ (a reference member's
        // constness is independent of its enclosing object's) -- see
        // `this_type.is_mutable_ref`'s own comment below for why the
        // "call" method's receiver itself is unconditionally mutable.
        std::unordered_set<std::string> by_value_names;
        for (LambdaCapture& capture : expr.lambda_captures) {
            captured_names.insert(capture.name);
            Type captured_type;
            if (capture.name == "this") {
                if (!enclosing_this_type.has_value()) {
                    throw DataflowError(
                        "a lambda captures 'this', but is not itself inside a method body (ch05 §5.12)",
                        expr.loc);
                }
                captured_type = *enclosing_this_type;
            } else if (capture.init) {
                std::optional<Type> t = infer_expr_type(*capture.init, enclosing_body, signatures_);
                if (!t.has_value()) {
                    throw DataflowError("cannot determine the type of init-capture '" + capture.name +
                                             "' (ch05 §5.12)",
                        expr.loc);
                }
                captured_type = std::move(*t);
            } else {
                auto it = enclosing_body.local_types.find(capture.name);
                if (it == enclosing_body.local_types.end()) {
                    throw DataflowError("lambda captures '" + capture.name +
                                             "', which is not a local variable or parameter in this scope (ch05 "
                                             "§5.12)",
                        expr.loc);
                }
                captured_type = it->second;
            }
            if (capture.by_reference) {
                Type ref;
                ref.kind = TypeKind::Reference;
                ref.pointee = std::make_shared<Type>(std::move(captured_type));
                // v1 simplification: every by-reference capture is a
                // mutable reference field, regardless of how the body
                // itself uses it -- ch05 §5.12 doesn't ask for a
                // separate const-vs-mutable capture distinction, and
                // real C++ itself doesn't track per-capture constness
                // this way either (a lambda's own constness -- the
                // `mutable` keyword -- is about by-*value* captures, see
                // `this_param.type.is_mutable_ref` below).
                ref.is_mutable_ref = true;
                field_types.push_back(std::move(ref));
            } else {
                if (capture.name != "this") by_value_names.insert(capture.name);
                field_types.push_back(std::move(captured_type));
            }
        }

        std::string class_name = "__lambda" + std::to_string(lambda_counter_++);
        ClassDef closure_class;
        closure_class.name = class_name;
        closure_class.fields.reserve(expr.lambda_captures.size());
        for (size_t i = 0; i < expr.lambda_captures.size(); i++) {
            ClassField field;
            field.name = expr.lambda_captures[i].name;
            field.type = field_types[i];
            field.access = AccessSpecifier::Private;
            closure_class.fields.push_back(std::move(field));
        }
        program_.classes.push_back(std::move(closure_class));

        Function call_method;
        call_method.name = class_name + "_call";
        call_method.loc = expr.loc;
        Param this_param;
        this_param.name = "this";
        Type this_type;
        this_type.kind = TypeKind::Reference;
        this_type.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = class_name});
        // The "call" method's own receiver is unconditionally mutable --
        // *not* gated by `mutable` (unlike a real C++ closure's
        // internally-const `operator()`): the general const-`this`-
        // propagation mechanism this would otherwise rely on
        // (assignment_target_is_read_only) cannot, without full
        // Program-wide field-type information movecheck's Body-based
        // architecture doesn't carry, distinguish "writing to a
        // by-value field" (should require `mutable`) from "writing
        // *through* a by-reference field's own referent" (must always
        // be allowed, matching real C++, where a reference member's
        // constness is independent of its enclosing object's). Instead,
        // "requires `mutable` to modify a by-value capture" is enforced
        // directly and precisely below
        // (reject_write_to_nonmutable_by_value_capture), using the
        // capture-list information only resolve_lambda itself has.
        this_type.is_mutable_ref = true;
        this_param.type = std::move(this_type);
        call_method.params.push_back(std::move(this_param));
        for (const Param& p : expr.lambda_params) call_method.params.push_back(p);

        call_method.body = expr.lambda_body ? clone_stmt(*expr.lambda_body) : nullptr;
        // ch05 §5.12: "a by-value capture can't be reassigned inside the
        // body" absent `mutable` -- checked on the *original* (pre field-
        // access-rewrite) body, where a captured name is still an
        // ordinary bare Identifier, so no field-type information is
        // needed (see the function's own comment).
        if (call_method.body && !expr.lambda_is_mutable) {
            reject_write_to_nonmutable_by_value_capture(*call_method.body, by_value_names);
        }
        // Return-type inference must likewise run on the *original*
        // (pre-rewrite) body: infer_expr_type has no Program access to
        // resolve a field's type from a `this.name` Member node (see
        // infer_lambda_return_type's own comment), but a captured name
        // is still a plain, resolvable Identifier at this point --
        // exactly like reject_write_to_nonmutable_by_value_capture's
        // identical reasoning just above.
        if (expr.has_lambda_explicit_return_type) {
            call_method.return_type = expr.type;
        } else if (call_method.body) {
            std::unordered_map<std::string, Type> capture_types;
            for (size_t i = 0; i < expr.lambda_captures.size(); i++) {
                capture_types[expr.lambda_captures[i].name] = field_types[i];
            }
            call_method.return_type = infer_lambda_return_type(*call_method.body, call_method.params, capture_types);
        } else {
            call_method.return_type = Type{.kind = TypeKind::Named, .name = "void"};
        }
        if (call_method.body) rewrite_captured_identifiers_as_field_access(*call_method.body, captured_names);

        // scpp requires an explicit `return;` covering every path, even
        // for a `void` function with an otherwise-empty body (verified
        // against this codebase's own existing behavior -- e.g. a bare
        // `Circle() {}` constructor is rejected the same way) -- real
        // C++ lambdas need no such thing (`[](int x) { print_int(x); }`
        // is perfectly valid with no `return` at all), so this
        // synthesis step must compensate by appending one when the
        // resolved return type is `void` and the body doesn't already
        // end with a Return statement (the common case for a body with
        // no explicit `-> Type` and no `return expr;` of its own -- a
        // more complex void body already ending in its own `return;` on
        // every path is left untouched, matching this same "don't guess,
        // defer to the real check" spirit codegen's own is_bare_void
        // helper follows elsewhere (not reusable here directly -- a
        // separate module, see this file's other independently-
        // duplicated helpers, e.g. types_equal).
        bool return_type_is_void =
            call_method.return_type.kind == TypeKind::Named && call_method.return_type.name == "void";
        if (return_type_is_void && call_method.body && call_method.body->kind == StmtKind::Block &&
            (call_method.body->statements.empty() ||
             call_method.body->statements.back()->kind != StmtKind::Return)) {
            auto return_stmt = std::make_unique<Stmt>();
            return_stmt->kind = StmtKind::Return;
            return_stmt->loc = expr.loc;
            call_method.body->statements.push_back(std::move(return_stmt));
        }

        program_.functions.push_back(std::move(call_method));
        Function& synthesized = program_.functions.back();

        expr.name = class_name;

        // Recurse into the synthesized method's own body (nested generic
        // calls / nested lambdas) using its own freshly-built Body --
        // capture fields are reached via `this.field` (a Member
        // expression, resolved structurally like any other class field,
        // never through body.local_types), so nothing about this
        // recursive walk needs to know about them specially. A
        // synthesized closure's own "call" method is never itself a
        // generic template, so generic-call-monomorphization stays
        // enabled here.
        if (synthesized.body) {
            Body synthesized_body = build_mir(synthesized);
            walk_stmt(*synthesized.body, synthesized_body, this_type_of(synthesized),
                      /*allow_generic_monomorphization=*/true);
        }
    }

    // ch05 §5.12: infers a lambda's return type from a single top-level
    // `return expr;` statement when no explicit trailing `-> Type` is
    // given (scpp has no general type inference, so this is a
    // deliberately narrow special case, matching the parser's own
    // comment) -- looks only at the body's own top-level Block
    // statements (not nested inside an If/While), mirroring how
    // narrowly this inference is meant to apply. No qualifying return
    // statement (none at all, or only a bare `return;`) infers `void`.
    // Ambiguous (more than one differently-shaped top-level return, or
    // a top-level return whose own expression type can't be determined
    // structurally) is left as `void` too, rather than guessing -- a
    // genuinely ambiguous case should use an explicit `-> Type` instead;
    // this is intentionally not a general control-flow analysis.
    // `call_params` is the synthesized "call" method's own params
    // (including `this`); `capture_types` maps each captured name to
    // its own resolved field type. Run on the *original* (pre field-
    // access-rewrite) body -- a captured name is still a plain bare
    // Identifier at this point (never yet a `this.field` Member access,
    // which infer_expr_type could not resolve anyway -- it has no
    // Program access to look up a field's type) -- so both a lambda's
    // own params and its captures are plain Identifiers infer_expr_type
    // can resolve directly from a fresh, flat Body (no enclosing
    // Function exists yet to build_mir from).
    [[nodiscard]] Type infer_lambda_return_type(const Stmt& body, const std::vector<Param>& call_params,
                                                 const std::unordered_map<std::string, Type>& capture_types) {
        if (body.kind != StmtKind::Block) return Type{.kind = TypeKind::Named, .name = "void"};
        Body param_only_body;
        for (const Param& p : call_params) {
            param_only_body.local_types[p.name] = p.type;
        }
        for (const auto& [name, type] : capture_types) {
            param_only_body.local_types[name] = type;
        }
        for (const StmtPtr& stmt : body.statements) {
            if (stmt->kind != StmtKind::Return || !stmt->expr) continue;
            // `[this]() { return this->value; }` (ch05 §5.12): a
            // `this`-capture's own field access -- infer_expr_type's
            // Member case can never resolve this (no Program access),
            // but this function, being a Monomorphizer method, has
            // `program_` directly -- special-cased here rather than
            // widening infer_expr_type's own general contract.
            if (stmt->expr->kind == ExprKind::Member && stmt->expr->lhs->kind == ExprKind::Identifier) {
                auto base_it = param_only_body.local_types.find(stmt->expr->lhs->name);
                if (base_it != param_only_body.local_types.end()) {
                    const Type& base_type = base_it->second;
                    const std::string& class_name =
                        (base_type.kind == TypeKind::Reference ? *base_type.pointee : base_type).name;
                    if (std::optional<Type> field_type = resolve_field_type(class_name, stmt->expr->name)) {
                        return *field_type;
                    }
                }
            }
            std::optional<Type> t = infer_expr_type(*stmt->expr, param_only_body, signatures_);
            if (t.has_value()) return *t;
            return Type{.kind = TypeKind::Named, .name = "void"};
        }
        return Type{.kind = TypeKind::Named, .name = "void"};
    }

    // Looks up `class_or_struct_name`'s own declared field `field_name`'s
    // type -- a Monomorphizer method, so it has direct `program_` access
    // (unlike movecheck's own, otherwise Program-less, Body-based
    // machinery -- see DataflowState::class_field_types for the parallel
    // mechanism check_moves needs for the exact same underlying reason).
    [[nodiscard]] std::optional<Type> resolve_field_type(const std::string& class_or_struct_name,
                                                          const std::string& field_name) const {
        for (const ClassDef& def : program_.classes) {
            if (def.name != class_or_struct_name) continue;
            for (const ClassField& field : def.fields) {
                if (field.name == field_name) return field.type;
            }
        }
        for (const StructDef& def : program_.structs) {
            if (def.name != class_or_struct_name) continue;
            for (const StructField& field : def.fields) {
                if (field.name == field_name) return field.type;
            }
        }
        return std::nullopt;
    }

    std::string get_or_create_clone(const Function& tmpl, const std::vector<Type>& concrete_param_types) {
        std::string cache_key = tmpl.name;
        for (const Type& t : concrete_param_types) cache_key += "." + mangle_type_for_clone_name(t);
        auto cached = clone_cache_.find(cache_key);
        if (cached != clone_cache_.end()) return cached->second;
        // Reserve the name *before* recursing (cloning tmpl's own body
        // below never re-enters get_or_create_clone for this exact same
        // key, but keeping this assignment first is simpler to reason
        // about than proving that independently).
        clone_cache_[cache_key] = cache_key;

        Function clone;
        clone.return_type = tmpl.return_type;
        clone.name = cache_key;
        clone.loc = tmpl.loc;
        clone.namespace_path = tmpl.namespace_path;
        // A monomorphized instantiation is always an internal
        // implementation detail of whatever called it -- never itself
        // directly exported (ch11 §11.3 doesn't apply to a compiler-
        // synthesized clone with a compiler-synthesized name).
        clone.is_exported = false;
        clone.params.reserve(tmpl.params.size());
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            Param p;
            p.name = tmpl.params[i].name;
            p.type = concrete_param_types[i];
            clone.params.push_back(std::move(p));
        }
        clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
        // is_generic_template stays false (default): the clone is an
        // ordinary, fully concrete function from here on, checked
        // normally by movecheck (see monomorphize_generics's own
        // comment) and compiled normally by codegen.

        program_.functions.push_back(std::move(clone));
        return cache_key;
    }
};

} // namespace
} // namespace scpp

export namespace scpp {

// ch05 §5.11: monomorphizes every call to a generic (concept-
// constrained) function in `program`, mutating it in place -- see
// Monomorphizer's own comment for the full algorithm and why this must
// run *before* check_moves (driver.cppm calls this first).
void monomorphize_generics(Program& program) {
    Monomorphizer monomorphizer(program);
    monomorphizer.run();
}

void check_moves(const Program& program) {
    Signatures signatures = build_signatures(program);
    // ch04 §4.2: every class name in the program, so Member-access
    // checking (apply_expr's own Member case) can tell a class-typed
    // base (access-controlled) apart from a struct-typed one (never
    // access-controlled, ch04 §4.1) -- see DataflowState::class_names.
    std::unordered_set<std::string> class_names;
    for (const ClassDef& def : program.classes) {
        class_names.insert(def.name);
    }
    // See DataflowState::class_field_types' own comment.
    ClassFieldTypes class_field_types;
    for (const ClassDef& def : program.classes) {
        for (const ClassField& field : def.fields) {
            class_field_types[def.name][field.name] = field.type;
        }
    }
    for (const StructDef& def : program.structs) {
        for (const StructField& field : def.fields) {
            class_field_types[def.name][field.name] = field.type;
        }
    }
    for (const Function& fn : program.functions) {
        // A bodyless `extern "C"` declaration (ch02 §2.1) has no
        // statements to run the dataflow analysis over -- it's already
        // registered in `signatures` above (so call sites into it are
        // still checked normally), but there's nothing here to check.
        if (!fn.body) continue;
        check_function(fn, signatures, class_names, class_field_types);
    }
}

} // namespace scpp
