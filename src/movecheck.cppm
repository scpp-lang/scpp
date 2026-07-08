module;

#include <algorithm>
#include <deque>
#include <functional>
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

struct ClosureCaptureBorrow {
    std::string root;
    bool is_mutable = false;

    bool operator==(const ClosureCaptureBorrow&) const = default;
};

using ClosureCaptureBorrowMap = std::unordered_map<std::string, std::vector<ClosureCaptureBorrow>>;

// Every struct/class's own declared field name -> Type, across the whole
// program -- see DataflowState::class_field_types' own comment for why
// this exists (movecheck's Body-only architecture otherwise has no way
// to resolve a Member expression's type).
using ClassFieldTypes = std::unordered_map<std::string, std::unordered_map<std::string, Type>>;

// ch04 §4.2: every *class*'s own declared field name -> its own
// AccessSpecifier, across the whole program -- see
// DataflowState::class_field_access's own comment. Never populated for a
// struct (struct fields have no access control at all, ch04 §4.1 -- see
// check_moves, which only fills this in for program.classes).
using ClassFieldAccess = std::unordered_map<std::string, std::unordered_map<std::string, AccessSpecifier>>;

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
    ClosureCaptureBorrowMap closure_capture_borrows;
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
    // ch04 §4.2: every *class*'s own declared field name -> its own
    // AccessSpecifier, across the whole program -- built once by
    // check_moves (mirrors class_field_types' identical lifetime/
    // non-null-once-set contract). Needed since a member variable may
    // now be `public` or `private` in any combination (real, unrestricted
    // C++ access control) -- apply_expr's Member case looks up the
    // accessed field's own access level here to decide whether external
    // access (from outside the class's own methods) is allowed (public)
    // or rejected (private), rather than unconditionally rejecting every
    // external field access the way an earlier, stricter version of this
    // rule did.
    const ClassFieldAccess* class_field_access = nullptr;
    // spec §6.5: every class name that has a copy constructor (user-
    // declared or compiler-eligible) / copy assignment operator,
    // respectively -- built once by check_moves (mirrors class_names'
    // identical lifetime/non-null-once-set contract). Needed to decide
    // whether `ClassName y = x;` / `y(x)` (copy construction) and
    // `y = x;` (copy assignment) are licensed at all for a given class,
    // and whether they dispatch to a user-declared function or the
    // compiler-synthesized memberwise one (see is_copy_constructible/
    // is_copy_assignable and has_user_declared_copy_ctor/copy_assign).
    const std::unordered_set<std::string>* classes_with_copy_ctor = nullptr;
    const std::unordered_set<std::string>* classes_with_copy_assign = nullptr;
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
               closure_capture_borrows == other.closure_capture_borrows &&
               unsafe_depth == other.unsafe_depth && current_class == other.current_class &&
               class_names == other.class_names && class_field_types == other.class_field_types &&
               class_field_access == other.class_field_access &&
               classes_with_copy_ctor == other.classes_with_copy_ctor &&
               classes_with_copy_assign == other.classes_with_copy_assign;
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
    // True only for a bodyless `extern "C"` declaration. An `extern "C"`
    // definition with a body is an ordinary checked function that merely
    // requests C linkage; only the declaration-only form is always
    // unchecked and therefore gated.
    bool is_extern_c_declaration_only = false;
    // Mirrors Function::is_unsafe (ch01 §1.2/§1.3) -- the function-
    // level `[[scpp::unsafe]]` marker on this specific overload's own
    // declaration: calling it is one more of ch05 §5.5's gated
    // operations, rejected unless the call site's own
    // DataflowState::unsafe_depth is greater than zero, exactly like
    // is_extern_c above (see check_call_arguments) -- scpp's equivalent
    // of Rust's `unsafe fn`.
    bool is_unsafe = false;
    // Where this specific overload's declaration begins -- see
    // Function::loc; needed for diagnostics that are about one
    // particular overload (e.g. a redefinition error naming the
    // conflicting declaration).
    SourceLocation loc;
    ReceiverRefQualifier receiver_ref_qualifier = ReceiverRefQualifier::None;
};

// ch05 §5.10: a name (free function or method, post class-mangling --
// see resolve_callee_signature) may now have *multiple* FunctionSignature
// entries -- one per overload -- instead of exactly one. Resolving a
// specific call to the single matching overload (exact type match only,
// ch06: no scpp scalar type implicitly converts to any other) is
// resolve_overload's job; nothing here assumes "one entry per name" any
// longer.
using Signatures = std::unordered_map<std::string, std::vector<FunctionSignature>>;

[[nodiscard]] bool is_thread_movable(const Type& type, std::unordered_set<std::string> visiting = {});
[[nodiscard]] bool is_thread_shareable(const Type& type, std::unordered_set<std::string> visiting = {});

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

ClosureCaptureBorrowMap join_closure_capture_borrows(const ClosureCaptureBorrowMap& a, const ClosureCaptureBorrowMap& b) {
    ClosureCaptureBorrowMap result = a;
    for (const auto& [name, borrows] : b) {
        result.insert_or_assign(name, borrows);
    }
    return result;
}

DataflowState join_states(const DataflowState& a, const DataflowState& b) {
    return DataflowState{
        join_maps(a.locals, b.locals),
        join_borrow_maps(a.borrows, b.borrows),
        join_ref_targets(a.ref_targets, b.ref_targets),
        join_closure_capture_borrows(a.closure_capture_borrows, b.closure_capture_borrows),
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
        a.class_field_access,
        // Same "set once, never changes, no real join needed" reasoning
        // as class_names/class_field_types/class_field_access just
        // above.
        a.classes_with_copy_ctor,
        a.classes_with_copy_assign,
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

[[nodiscard]] bool is_reference(const Type& type) { return type.kind == TypeKind::Reference; }
[[nodiscard]] bool is_span(const Type& type) { return type.kind == TypeKind::Span; }
[[nodiscard]] bool is_function_pointer(const Type& type) { return type.kind == TypeKind::FunctionPointer; }
[[nodiscard]] bool is_explicit_star_this(const Expr& expr) {
    return expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr &&
           expr.lhs->kind == ExprKind::Identifier && expr.lhs->name == "this";
}

// ch06 §6: the complete scalar/numeric family a `static_cast<T>(expr)`/
// `(T)expr` may legally convert between (ExprKind::Cast's own apply_expr
// case) -- `TypeKind::Named` alone isn't enough to tell a scalar apart
// from a struct/class/witness name (all three share that TypeKind), so
// this checks against the exact, closed set ch06 documents rather than
// the type's own `kind`.
[[nodiscard]] bool is_scalar_type_name(const std::string& name) {
    static const std::unordered_set<std::string> scalar_names = {
        "bool", "char", "int", "long", "unsigned int", "unsigned long", "int8_t", "int16_t", "int32_t",
        "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double", "float32_t", "float64_t",
        "size_t", "ptrdiff_t"};
    return scalar_names.contains(name);
}

// spec §6.5: whether `class_name` has declared its own copy constructor
// -- a function named "class_name_new" (see parse_class_def) whose sole
// non-`this` parameter is `const class_name&` (an ordinary, non-rvalue,
// read-only reference to the class's own type -- the shape spec §6.5's
// own worked example, and the overwhelmingly common real-world one,
// uses; a mutable-reference-parameter copy constructor, while legal
// real C++, is out of scope for this recognition).
[[nodiscard]] bool has_user_declared_copy_ctor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (!fn.name.ends_with("_new") || fn.params.size() != 2) continue;
        const Type& this_param = fn.params[0].type;
        if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
            this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
            continue;
        }
        const Type& p = fn.params[1].type;
        if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
            p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
            return true;
        }
    }
    return false;
}

// spec §6.5: whether `class_name` has declared its own copy assignment
// operator -- a function named "class_name_operator_assign" (see
// parse_class_body_into's operator= parsing) whose sole non-`this`
// parameter is `const class_name&`, mirroring has_user_declared_copy_ctor
// exactly (an operator= overload taking any other shape is simply an
// ordinary, unrelated overload of the name -- not *the* copy assignment
// operator this recognizes).
[[nodiscard]] bool has_user_declared_copy_assign(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (!fn.name.ends_with("_operator_assign") || fn.params.size() != 2) continue;
        const Type& this_param = fn.params[0].type;
        if (this_param.kind != TypeKind::Reference || !this_param.is_mutable_ref || !this_param.pointee ||
            this_param.pointee->kind != TypeKind::Named || this_param.pointee->name != class_name) {
            continue;
        }
        const Type& p = fn.params[1].type;
        if (p.kind == TypeKind::Reference && !p.is_rvalue_ref && !p.is_mutable_ref && p.pointee &&
            p.pointee->kind == TypeKind::Named && p.pointee->name == class_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_user_declared_dtor(const std::string& class_name, const Program& program) {
    for (const Function& fn : program.functions) {
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) continue;
        const Type& this_param = fn.params[0].type;
        if (this_param.kind == TypeKind::Reference && this_param.is_mutable_ref && this_param.pointee &&
            this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == class_name) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program);
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program);

// spec §6.5(5)'s own note: a field's own copy-constructibility -- a
// reference always is (bound once, from the source's own referent,
// exactly like move construction's identical carve-out, spec §6.4); a
// nested class recurses; everything else (scalar, struct -- always
// bitwise-copyable per ch04 §4.1 regardless of its own fields, raw
// pointer, array of any of these) is unconditionally
// copy-constructible.
[[nodiscard]] bool is_field_copy_constructible(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return true;
    if (type.kind == TypeKind::Array) return is_field_copy_constructible(*type.element, program);
    if (type.kind == TypeKind::Named) {
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_constructible(type.name, program);
        }
        return true; // scalar, struct, or an unrecognized/generic-witness name
    }
    return true; // Pointer, Span, ...: always bitwise-copyable, no restriction
}

// Same as is_field_copy_constructible, but for assignment -- a reference
// field is the one case that differs (never assignable, spec §6.4/§6.5's
// identical "can't be re-seated" carve-out); is_copy_assignable's own
// direct-field loop already rejects a *directly* reference-typed field
// before ever consulting this helper, but nested recursion still needs
// its own answer for one reachable transitively (impossible in the
// current v0.1 subset, since a struct/class field can never itself be
// reference-typed except via this exact direct case -- kept anyway for
// symmetry and defensiveness).
[[nodiscard]] bool is_field_copy_assignable(const Type& type, const Program& program) {
    if (type.kind == TypeKind::Reference) return false;
    if (type.kind == TypeKind::Array) return is_field_copy_assignable(*type.element, program);
    if (type.kind == TypeKind::Named) {
        for (const ClassDef& def : program.classes) {
            if (def.name == type.name) return is_copy_assignable(type.name, program);
        }
        return true;
    }
    return true;
}

// spec §6.5(2): a class has an implicitly-defined copy constructor iff
// it declares none of {copy constructor, copy assignment operator,
// destructor} itself (ch08 Q15's "no mixed state" tightening) *and*
// every field is itself copy-constructible (spec §6.5(5)'s own
// recursive note) -- a user-declared copy constructor always wins
// regardless of fields (it's the user's own code, not compiler-derived).
[[nodiscard]] bool is_copy_constructible(const std::string& class_name, const Program& program) {
    if (has_user_declared_copy_ctor(class_name, program)) return true;
    if (has_user_declared_dtor(class_name, program) || has_user_declared_copy_assign(class_name, program)) {
        return false;
    }
    for (const ClassDef& def : program.classes) {
        if (def.name != class_name) continue;
        for (const ClassField& f : def.fields) {
            if (!is_field_copy_constructible(f.type, program)) return false;
        }
        return true;
    }
    return false; // not a recognized class at all
}

// spec §6.5(3): symmetric to is_copy_constructible, plus the reference-
// member carve-out (a class with a directly reference-typed field has
// no compiler-provided copy assignment operator at all, exactly
// mirroring move assignment's identical spec §6.4(3) rule).
[[nodiscard]] bool is_copy_assignable(const std::string& class_name, const Program& program) {
    if (has_user_declared_copy_assign(class_name, program)) return true;
    if (has_user_declared_dtor(class_name, program) || has_user_declared_copy_ctor(class_name, program)) {
        return false;
    }
    for (const ClassDef& def : program.classes) {
        if (def.name != class_name) continue;
        for (const ClassField& f : def.fields) {
            if (is_reference(f.type)) return false;
            if (!is_field_copy_assignable(f.type, program)) return false;
        }
        return true;
    }
    return false;
}

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
            if (a.name != b.name || a.template_args.size() != b.template_args.size()) return false;
            for (size_t i = 0; i < a.template_args.size(); i++) {
                if (!types_equal(a.template_args[i], b.template_args[i])) return false;
            }
            return true;
        case TypeKind::Pointer:
            return a.is_mutable_pointee == b.is_mutable_pointee && types_equal(*a.pointee, *b.pointee);
        case TypeKind::Function:
        case TypeKind::FunctionPointer:
            if ((a.kind == TypeKind::FunctionPointer && a.is_unsafe_function_pointer != b.is_unsafe_function_pointer) ||
                (a.kind == TypeKind::Function &&
                 (a.is_const_function != b.is_const_function ||
                  a.function_ref_qualifier != b.function_ref_qualifier)) ||
                !types_equal(*a.function_return, *b.function_return) ||
                a.function_params.size() != b.function_params.size()) {
                return false;
            }
            for (size_t i = 0; i < a.function_params.size(); i++) {
                if (!types_equal(a.function_params[i], b.function_params[i])) return false;
            }
            return true;
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

[[nodiscard]] bool raw_pointer_implicitly_convertible(const Type& source, const Type& target) {
    if (source.kind != TypeKind::Pointer || target.kind != TypeKind::Pointer) return false;
    if (!source.is_mutable_pointee && target.is_mutable_pointee) return false;
    const Type& source_pointee =
        source.pointee->kind == TypeKind::Reference && source.pointee->pointee ? *source.pointee->pointee : *source.pointee;
    const Type& target_pointee =
        target.pointee->kind == TypeKind::Reference && target.pointee->pointee ? *target.pointee->pointee : *target.pointee;
    if (types_equal(source_pointee, target_pointee)) return true;
    bool source_is_void = source_pointee.kind == TypeKind::Named && source_pointee.name == "void";
    bool target_is_void = target_pointee.kind == TypeKind::Named && target_pointee.name == "void";
    return source_is_void || target_is_void;
}

[[nodiscard]] bool is_scalar_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "int" || type.name == "bool" || type.name == "char" || type.name == "long" ||
            type.name == "float" || type.name == "double" || type.name == "unsigned int" ||
            type.name == "unsigned long" || type.name == "size_t" || type.name == "ptrdiff_t" ||
            type.name == "int8_t" || type.name == "int16_t" || type.name == "int32_t" || type.name == "int64_t" ||
            type.name == "uint8_t" || type.name == "uint16_t" || type.name == "uint32_t" || type.name == "uint64_t" ||
            type.name == "float32_t" || type.name == "float64_t");
}

[[nodiscard]] bool is_float_named_type(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "float" || type.name == "double" || type.name == "float32_t" || type.name == "float64_t");
}

[[nodiscard]] bool integer_literal_compatible_with_type(const Type& type) {
    return type.kind == TypeKind::Named && type.name != "bool" && type.name != "char" && is_scalar_named_type(type);
}

[[nodiscard]] const Type& binary_operand_type(const Type& type) {
    return type.kind == TypeKind::Reference ? *type.pointee : type;
}

[[nodiscard]] bool literal_compatible_with_type(const Expr& literal, const Type& type) {
    const Type& operand_type = binary_operand_type(type);
    switch (literal.kind) {
        case ExprKind::IntegerLiteral: return integer_literal_compatible_with_type(operand_type);
        case ExprKind::FloatLiteral: return is_float_named_type(operand_type);
        case ExprKind::BoolLiteral: return operand_type.kind == TypeKind::Named && operand_type.name == "bool";
        case ExprKind::CharLiteral: return operand_type.kind == TypeKind::Named && operand_type.name == "char";
        default: return false;
    }
}

// A Call expression's signature-lookup key, plus how many leading
// `signatures[key].param_types` entries are already spoken for before
// `expr.args[0]` (1 when an implicit `this` occupies param_types[0], 0
// otherwise).
struct CalleeSignature {
    std::string key;
    size_t param_offset = 0;
    std::optional<FunctionSignature> direct_signature;
};

[[nodiscard]] FunctionSignature function_pointer_signature(const Type& type) {
    FunctionSignature sig;
    sig.param_types = type.function_params;
    sig.return_type = *type.function_return;
    sig.is_unsafe = type.is_unsafe_function_pointer;
    return sig;
}

// Resolves a Call expression's signature-lookup key, accounting for a
// method call's receiver (ch04 §4.2/ch05 §5.9): `obj.name(...)`/
// `this->name(...)` stores its receiver in `call_expr.lhs` and only the
// unqualified method name in `call_expr.name`, but `signatures` (like
// codegen's own `module_->getFunction`) is keyed by the synthesized
// `ClassName_methodName` form (see parse_class_def) -- exactly like
// codegen_call independently resolves the same fact from the receiver's
// type. Scoped to a plain Identifier receiver (covers `this->method()`
// and `obj.method()` for a local/parameter `obj`), a Lambda literal
// receiver (ch05 §5.12's IIFE, e.g. `[](int x){...}(5)` -- already
// resolved to its own synthesized closure class name by the time
// check_moves runs, see monomorphize_generics), or -- only when
// `class_field_types` is supplied (optional: most callers have no
// Program-level field-type info to give it, see DataflowState::
// class_field_types' own comment) -- one more Member projection off a
// plain-Identifier base (`this.field.method()`/`obj.field.method()`),
// resolved through the field's own declared type. A more complex
// receiver expression still falls back to the unqualified name and a
// zero offset, same as an ordinary free-function call (this is *not* a
// general type-checker). Shared by check_call_arguments and
// produces_rvalue_of_type so both resolve a method call's callee
// identically.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                  DataflowState& state, const Body& body, const Signatures& signatures,
                                  bool report_errors);
void maybe_instantiate_generic_constructor_overloads(const std::string& class_name,
                                                      const std::vector<ExprPtr>& args, Body& body,
                                                      SourceLocation loc);
[[nodiscard]] CalleeSignature resolve_callee_signature(const Expr& call_expr, const Body& body,
                                                        const ClassFieldTypes* class_field_types = nullptr) {
    if (call_expr.lhs && call_expr.name.empty()) {
        const Expr* callee_expr = call_expr.lhs.get();
        if (callee_expr->kind == ExprKind::Unary && callee_expr->unary_op == UnaryOp::Deref && callee_expr->lhs) {
            callee_expr = callee_expr->lhs.get();
        }
        if (callee_expr->kind == ExprKind::Identifier) {
            auto type_it = body.local_types.find(callee_expr->name);
            if (type_it != body.local_types.end() && is_function_pointer(type_it->second)) {
                return CalleeSignature{"", 0, function_pointer_signature(type_it->second)};
            }
        } else if (class_field_types != nullptr && callee_expr->kind == ExprKind::Member && callee_expr->lhs &&
                   callee_expr->lhs->kind == ExprKind::Identifier) {
            auto base_it = body.local_types.find(callee_expr->lhs->name);
            if (base_it != body.local_types.end()) {
                const Type& base_type =
                    base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
                if (base_type.kind == TypeKind::Named) {
                    auto fields_it = class_field_types->find(base_type.name);
                    if (fields_it != class_field_types->end()) {
                        auto field_it = fields_it->second.find(callee_expr->name);
                        if (field_it != fields_it->second.end() && is_function_pointer(field_it->second)) {
                            return CalleeSignature{"", 0, function_pointer_signature(field_it->second)};
                        }
                    }
                }
            }
        }
    }
    if (call_expr.lhs && !call_expr.name.empty() && class_field_types != nullptr &&
        call_expr.lhs->kind == ExprKind::Identifier) {
        auto base_it = body.local_types.find(call_expr.lhs->name);
        if (base_it != body.local_types.end()) {
            const Type& base_type =
                base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
            if (base_type.kind == TypeKind::Named) {
                auto fields_it = class_field_types->find(base_type.name);
                if (fields_it != class_field_types->end()) {
                    auto field_it = fields_it->second.find(call_expr.name);
                    if (field_it != fields_it->second.end() && is_function_pointer(field_it->second)) {
                        return CalleeSignature{"", 0, function_pointer_signature(field_it->second)};
                    }
                }
            }
        }
    }
    if (!call_expr.lhs && body.local_types.contains(call_expr.name) && is_function_pointer(body.local_types.at(call_expr.name))) {
        return CalleeSignature{"", 0, function_pointer_signature(body.local_types.at(call_expr.name))};
    }
    if (call_expr.lhs) {
        std::string class_name;
        if (call_expr.lhs->kind == ExprKind::Identifier) {
            auto type_it = body.local_types.find(call_expr.lhs->name);
            if (type_it != body.local_types.end()) class_name = named_type_name(type_it->second);
        } else if (is_explicit_star_this(*call_expr.lhs)) {
            auto type_it = body.local_types.find("this");
            if (type_it != body.local_types.end()) class_name = named_type_name(type_it->second);
        } else if (call_expr.lhs->kind == ExprKind::Lambda && !call_expr.lhs->name.empty()) {
            class_name = call_expr.lhs->name;
        } else if (class_field_types != nullptr && call_expr.lhs->kind == ExprKind::Member &&
                   call_expr.lhs->lhs && call_expr.lhs->lhs->kind == ExprKind::Identifier) {
            // ch05 §5.14: needed for check_generic_type_methods_once's
            // own synthesized check functions -- a generic type's method
            // calling another method *on one of its own fields*
            // (`this.item.doubled()`) must still be resolved (and, when
            // `item`'s substituted type turns out to guarantee no such
            // method, correctly left unresolvable) even though the
            // receiver is a Member, not a bare Identifier -- otherwise
            // this falls back to an unqualified, unmangled lookup
            // ("doubled") that (almost) never matches anything real,
            // silently deferring an unresolvable call entirely to
            // codegen -- which never runs at all for a synthetic,
            // check-only function (ClassDef::is_synthetic_check_only),
            // the exact gap this closes.
            auto base_it = body.local_types.find(call_expr.lhs->lhs->name);
            if (base_it != body.local_types.end()) {
                const Type& base_type =
                    base_it->second.kind == TypeKind::Reference ? *base_it->second.pointee : base_it->second;
                if (base_type.kind == TypeKind::Named) {
                    auto fields_it = class_field_types->find(base_type.name);
                    if (fields_it != class_field_types->end()) {
                        auto field_it = fields_it->second.find(call_expr.lhs->name);
                        if (field_it != fields_it->second.end()) class_name = named_type_name(field_it->second);
                    }
                }
            }
        }
        if (class_name.empty()) {
            std::optional<Type> receiver_type = infer_expr_type(*call_expr.lhs, body, {});
            if (receiver_type.has_value()) class_name = named_type_name(*receiver_type);
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
//    parameter types), produces_rvalue_of_type (defined below), and
//    is_read_only_reachable (defined much further below, for the
//    T&-beats-const-T&-for-a-mutable-lvalue tie-break).
// All of this always terminates: every recursive step is into a strictly
// smaller sub-expression.
[[nodiscard]] std::optional<Type> infer_expr_type(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] const FunctionSignature* resolve_overload(const Expr& call_expr, const CalleeSignature& callee,
                                                          const Body& body, const Signatures& signatures);
[[nodiscard]] bool is_read_only_reachable(const Expr& expr, const Body& body, const Signatures& signatures);
[[nodiscard]] bool produces_rvalue_of_type(const Expr& expr, const Type& expected_type, const Body& body,
                                            const Signatures& signatures);
// spec §6.5: forward-declared since apply_expr's own Binary/Assign case
// (defined well before is_bare_same_type_copy_source's own definition,
// near is_move_construction_shape) needs it for the Member-target copy-
// assignment eligibility check.
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type, const Body& body);
[[nodiscard]] bool is_named_class_type(const Type& type, const Body& body) {
    if (type.kind != TypeKind::Named || body.program == nullptr) return false;
    for (const ClassDef& def : body.program->classes) {
        if (def.name == type.name) return !def.is_concept_witness;
    }
    return false;
}

[[nodiscard]] bool is_copyable_class_lvalue_boundary_source(const Expr& expr, const Type& target_type, const Body& body) {
    return body.program != nullptr && is_named_class_type(target_type, body) &&
           is_bare_same_type_copy_source(expr, target_type, body) &&
           is_copy_constructible(target_type.name, *body.program);
}

// Whether `arg` is a legitimate argument for a candidate overload's
// parameter declared as `param_type`, for exact-type-match overload
// resolution (ch05 §5.10) -- not a full validity check (that's
// check_call_arguments/apply_reference_argument's job, once a specific
// overload has already been picked); this only needs to decide which of
// several candidates is *the* match.
[[nodiscard]] bool argument_matches_parameter(const Expr& arg, const Type& param_type, const Body& body,
                                                const Signatures& signatures) {
    if (is_reference(param_type) && param_type.is_rvalue_ref) {
        // ch03/ch05 §5.11: `T&&`/`Concept auto&&` -- the mirror image of
        // the ordinary-reference case just below: needs a genuine
        // rvalue-producing argument, never a bare place.
        return produces_rvalue_of_type(arg, *param_type.pointee, body, signatures);
    }
    if (is_reference(param_type)) {
        // ch05 §5.x: a *const* reference may bind directly to a fresh
        // rvalue argument -- exactly like the `T&&` case just above,
        // just gated to only a non-mutable reference (real C++ forbids
        // binding a *mutable* lvalue reference to a temporary).
        if (!param_type.is_mutable_ref && produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) {
            return true;
        }
        // A bare lvalue-like place (Identifier/Member/Subscript/a
        // unique_ptr or raw pointer's Deref -- the same shapes
        // resolve_borrow_source_root accepts as a borrow source) is
        // viable against a T&/const T& parameter; std::move/MakeUnique/a
        // literal never is (there's no place to borrow from) unless the
        // rvalue-binding case just above already accepted it.
        if (arg.kind == ExprKind::Move ||
            arg.kind == ExprKind::IntegerLiteral || arg.kind == ExprKind::FloatLiteral ||
            arg.kind == ExprKind::BoolLiteral || arg.kind == ExprKind::CharLiteral ||
            arg.kind == ExprKind::StringLiteral) {
            return false;
        }
        std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
        return arg_type.has_value() && types_equal(*arg_type, *param_type.pointee);
    }
    std::optional<Type> arg_type = infer_expr_type(arg, body, signatures);
    if (!arg_type.has_value() || !types_equal(*arg_type, param_type)) return false;
    if (is_named_class_type(param_type, body)) {
        return is_copyable_class_lvalue_boundary_source(arg, param_type, body) ||
               produces_rvalue_of_type(arg, param_type, body, signatures);
    }
    return true;
}

[[nodiscard]] bool receiver_matches_method_qualifier(const Expr& receiver_expr, const FunctionSignature& candidate,
                                                     const Body& body, const Signatures& signatures) {
    if (candidate.param_types.empty() || candidate.param_types[0].kind != TypeKind::Reference ||
        candidate.param_types[0].pointee == nullptr) {
        return true;
    }
    bool receiver_is_rvalue =
        produces_rvalue_of_type(receiver_expr, *candidate.param_types[0].pointee, body, signatures);
    switch (candidate.receiver_ref_qualifier) {
        case ReceiverRefQualifier::None: return true;
        case ReceiverRefQualifier::LValue: return !receiver_is_rvalue;
        case ReceiverRefQualifier::RValue: return receiver_is_rvalue;
    }
    return true;
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
    if (callee.direct_signature.has_value()) {
        return callee.direct_signature->param_types.size() == call_expr.args.size() + callee.param_offset
                   ? &*callee.direct_signature
                   : nullptr;
    }
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
    if (it->second.size() == 1) {
        const FunctionSignature& only = it->second[0];
        if (callee.param_offset == 1 && call_expr.lhs) {
            if (!receiver_matches_method_qualifier(*call_expr.lhs, only, body, signatures)) return nullptr;
        }
        return &only;
    }

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
        if (all_match && callee.param_offset == 1 && call_expr.lhs &&
            !receiver_matches_method_qualifier(*call_expr.lhs, candidate, body, signatures)) {
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
        if (callee.param_offset == 1 && call_expr.lhs) {
            bool receiver_is_rvalue =
                candidate.param_types[0].kind == TypeKind::Reference && candidate.param_types[0].pointee != nullptr &&
                produces_rvalue_of_type(*call_expr.lhs, *candidate.param_types[0].pointee, body, signatures);
            if ((receiver_is_rvalue && candidate.receiver_ref_qualifier == ReceiverRefQualifier::RValue) ||
                (!receiver_is_rvalue && candidate.receiver_ref_qualifier == ReceiverRefQualifier::LValue)) {
                score += 2;
            }
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

[[nodiscard]] Type function_pointer_type_from_signature(const FunctionSignature& sig) {
    Type type;
    type.kind = TypeKind::FunctionPointer;
    type.function_return = std::make_shared<Type>(sig.return_type);
    type.function_params = sig.param_types;
    type.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
    return type;
}

[[nodiscard]] bool same_function_pointer_shape_ignoring_unsafe(const Type& a, const Type& b) {
    if (!is_function_pointer(a) || !is_function_pointer(b) || a.function_params.size() != b.function_params.size() ||
        !types_equal(*a.function_return, *b.function_return)) {
        return false;
    }
    for (size_t i = 0; i < a.function_params.size(); i++) {
        if (!types_equal(a.function_params[i], b.function_params[i])) return false;
    }
    return true;
}

[[nodiscard]] std::optional<Type> resolve_function_designator_type(const Expr& expr, const Type& target_type,
                                                                   const Body& body, const Signatures& signatures) {
    const Expr* source = &expr;
    if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::AddressOf && expr.lhs) source = expr.lhs.get();
    if (source->kind != ExprKind::Identifier || body.local_types.contains(source->name)) return std::nullopt;
    auto it = signatures.find(source->name);
    if (it == signatures.end()) return std::nullopt;
    for (const FunctionSignature& sig : it->second) {
        Type candidate = function_pointer_type_from_signature(sig);
        if (same_function_pointer_shape_ignoring_unsafe(candidate, target_type)) return candidate;
    }
    return std::nullopt;
}

void check_function_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                       const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                       bool report_errors) {
    if (!report_errors || !is_function_pointer(target_type)) return;
    std::optional<Type> source_type = resolve_function_designator_type(expr, target_type, body, signatures);
    if (!source_type) source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || !is_function_pointer(*source_type)) {
        throw DataflowError("cannot initialize function pointer '" + target_name +
                             "' from this expression: expected a function or function pointer with matching "
                             "signature",
            loc);
    }
    if (types_equal(target_type, *source_type)) return;
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && target_type.is_unsafe_function_pointer &&
        !source_type->is_unsafe_function_pointer) {
        return;
    }
    if (same_function_pointer_shape_ignoring_unsafe(target_type, *source_type) && !target_type.is_unsafe_function_pointer &&
        source_type->is_unsafe_function_pointer) {
        throw DataflowError("cannot assign an unsafe-qualified function pointer to plain function pointer '" +
                                 target_name + "'",
            loc);
    }
}

void check_raw_pointer_assignment(const Type& target_type, const Expr& expr, const Body& body,
                                   const Signatures& signatures, SourceLocation loc, const std::string& target_name,
                                   bool report_errors) {
    if (!report_errors || target_type.kind != TypeKind::Pointer) return;
    std::optional<Type> source_type = infer_expr_type(expr, body, signatures);
    if (!source_type || source_type->kind != TypeKind::Pointer) return;
    if (raw_pointer_implicitly_convertible(*source_type, target_type)) return;
    throw DataflowError("cannot initialize or assign raw pointer '" + target_name +
                            "' from an incompatible pointer type without an explicit cast",
                        loc);
    throw DataflowError("function pointer '" + target_name + "' has a different signature than this source expression", loc);
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

    // ch04 §4.2/ch05 §5.9/spec §6.5: a method's own `this` (always
    // params[0], see make_this_param) is *always* the elision source
    // when present, regardless of how many other reference parameters
    // the method also takes -- the "this-elision rule" other comments
    // in this file already reference by name. This isn't a general
    // multiple-reference-parameter lifetime-group solution (ch05 §5.3's
    // own `[[scpp::lifetime(name)]]` design remains unimplemented,
    // tracked past v0.1) -- it's a narrow, specifically-justified
    // special case for exactly the shape a user-declared copy
    // assignment operator needs (spec §6.5's own worked example,
    // `RefCounted& operator=(const RefCounted& other) { ...; return
    // *this; }`): real C++'s own universal convention is that an
    // assignment operator always returns `*this`, never its argument,
    // so `this` is the only sound choice regardless of what other
    // reference parameters are also in scope -- exactly like the
    // single-reference-parameter case below, this is a structural,
    // signature-level inference (never verified against what the body
    // actually returns), just extended to cover this one additional,
    // well-understood shape.
    if (!fn.params.empty() && fn.params[0].name == "this" && is_reference(fn.params[0].type)) {
        if (fn.return_type.is_mutable_ref && !fn.params[0].type.is_mutable_ref) {
            throw DataflowError("function '" + fn.name +
                                 "' returns a mutable reference ('T&') but its 'this' is a read-only ('const') "
                                 "receiver; a mutable reference cannot be manufactured from a shared one",
                fn.loc);
        }
        return 0;
    }

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
            if (it == body.local_types.end()) return false;
            if (expr.lhs->name == "this" && it->second.kind == TypeKind::Reference) {
                return !it->second.is_mutable_ref;
            }
            return it->second.kind == TypeKind::Pointer && !it->second.is_mutable_pointee;
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
            if (is_explicit_star_this(expr)) return "this";
            if (expr.unary_op != UnaryOp::Deref || expr.lhs->kind != ExprKind::Identifier) {
                return std::nullopt;
            }
            return expr.lhs->name;
        case ExprKind::Call:
            if (expr.name == "operator_deref" && expr.lhs != nullptr) {
                if (expr.lhs->kind == ExprKind::Identifier) return expr.lhs->name;
                if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                    return direct_write_root(*expr.lhs->lhs, body);
                }
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

// ch03/ch05 §5.11: the expressions allowed to bind to a `T&&` (rvalue-
// reference/move) parameter, checked against a specific `expected_type`.
// Reused, via the same Type::is_rvalue_ref flag, for a `Concept auto&&`
// generic parameter's own witness-typed slot (ch05 §5.11) and for
// passing a lambda expression literal to one (ch05 §5.12, once
// ExprKind::Lambda exists -- add it to the switch below at that point; a
// lambda literal is a fresh prvalue exactly like the cases already
// handled here). `std::move(x)` is allowed here when apply_expr's own
// Move-processing rules already license it for `x`; this helper only
// decides which *expression shapes* count as rvalues once that semantic
// check is separately satisfied.
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
        case ExprKind::New:
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
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
            if (sig == nullptr) {
                std::optional<Type> call_type = infer_expr_type(expr, body, signatures);
                if (!expr.lhs && call_type.has_value() && types_equal(*call_type, expected_type)) break;
                return false;
            }
            // A reference-returning call yields a place/alias, not a
            // fresh value (see resolve_borrow_source_root's own Call
            // case) -- legitimate as a T&/const T& source elsewhere, but
            // not here.
            if (is_reference(sig->return_type)) return false;
            break;
        }
        default:
            return false;
    }
    std::optional<Type> actual_type = infer_expr_type(expr, body, signatures);
    if (!actual_type.has_value()) return false;
    if (types_equal(*actual_type, expected_type)) return true;
    if (expr.kind == ExprKind::Move && actual_type->kind == TypeKind::Reference && actual_type->pointee != nullptr) {
        return types_equal(*actual_type->pointee, expected_type);
    }
    return false;
}

// Infers `expr`'s scpp type, for function-overload resolution purposes
// only (ch05 §5.10) -- a best-effort, non-exhaustive type inference
// (movecheck has no general type-checking pass at all, by design: see
// e.g. produces_rvalue_of_type's similarly-scoped Call handling just
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
        case ExprKind::FloatLiteral: return Type{.kind = TypeKind::Named, .name = "double"};
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
            if (it != body.local_types.end()) return it->second;
            auto sig_it = signatures.find(expr.name);
            if (sig_it != signatures.end() && sig_it->second.size() == 1) {
                const FunctionSignature& sig = sig_it->second[0];
                Type result;
                result.kind = TypeKind::FunctionPointer;
                result.function_return = std::make_shared<Type>(sig.return_type);
                result.function_params = sig.param_types;
                result.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
                return result;
            }
            return std::nullopt;
        }

        case ExprKind::Move: {
            // std::move doesn't change the static type -- still whatever
            // std::unique_ptr<T> the moved-from local was declared as.
            if (expr.lhs->kind != ExprKind::Identifier) return std::nullopt;
            auto it = body.local_types.find(expr.lhs->name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }

        case ExprKind::New: {
            Type result;
            result.kind = TypeKind::Pointer;
            result.pointee = std::make_shared<Type>(expr.type);
            result.is_mutable_pointee = true;
            return result;
        }

        case ExprKind::Delete:
            return Type{.kind = TypeKind::Named, .name = "void"};

        case ExprKind::TypeTrait:
            return Type{.kind = TypeKind::Named, .name = "bool"};

        // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): the cast's own
        // declared target type, unconditionally -- see codegen's
        // identical infer_type case.
        case ExprKind::Cast: return expr.type;

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
                    if (expr.lhs->kind == ExprKind::Identifier && !body.local_types.contains(expr.lhs->name)) {
                        auto it = signatures.find(expr.lhs->name);
                        if (it != signatures.end() && it->second.size() == 1) {
                            const FunctionSignature& sig = it->second[0];
                            Type result;
                            result.kind = TypeKind::FunctionPointer;
                            result.function_return = std::make_shared<Type>(sig.return_type);
                            result.function_params = sig.param_types;
                            result.is_unsafe_function_pointer = sig.is_unsafe || sig.is_extern_c_declaration_only;
                            return result;
                        }
                    }
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
                    if (!operand) return std::nullopt;
                    if (is_explicit_star_this(expr) && operand->kind == TypeKind::Reference && operand->pointee) {
                        return *operand->pointee;
                    }
                    if (is_function_pointer(*operand)) return *operand;
                    const Type& underlying =
                        operand->kind == TypeKind::Reference && operand->pointee ? *operand->pointee : *operand;
                    if (underlying.kind == TypeKind::Named) {
                        auto sig_it = signatures.find(underlying.name + "_operator_deref");
                        if (sig_it != signatures.end()) {
                            for (const FunctionSignature& sig : sig_it->second) {
                                if (sig.param_types.empty()) continue;
                                return sig.return_type.kind == TypeKind::Reference
                                           ? std::optional<Type>(*sig.return_type.pointee)
                                           : std::optional<Type>(sig.return_type);
                            }
                        }
                    }
                    if (operand->kind != TypeKind::Pointer) return std::nullopt;
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

        case ExprKind::Conditional: {
            std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
            std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
            if (!then_type.has_value() || !else_type.has_value()) return std::nullopt;
            return types_equal(*then_type, *else_type) ? then_type : std::nullopt;
        }

        case ExprKind::Fold:
            if (expr.rhs) return infer_expr_type(*expr.rhs, body, signatures);
            return infer_expr_type(*expr.lhs, body, signatures);

        case ExprKind::Call: {
            CalleeSignature callee = resolve_callee_signature(expr, body);
            const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
            if (sig != nullptr) return sig->return_type;
            if (expr.lhs == nullptr && body.program != nullptr) {
                for (const ClassDef& def : body.program->classes) {
                    if (def.name == expr.name) return Type{.kind = TypeKind::Named, .name = expr.name};
                }
                for (const StructDef& def : body.program->structs) {
                    if (def.name == expr.name) return Type{.kind = TypeKind::Named, .name = expr.name};
                }
            }
            return std::nullopt;
        }

        case ExprKind::PackExpansion:
            return std::nullopt;

        case ExprKind::Member: {
            std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
            if (base_named.kind != TypeKind::Named || body.program == nullptr) return std::nullopt;
            for (const ClassDef& def : body.program->classes) {
                if (def.name != base_named.name) continue;
                for (const ClassField& field : def.fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return std::nullopt;
            }
            for (const StructDef& def : body.program->structs) {
                if (def.name != base_named.name) continue;
                for (const StructField& field : def.fields) {
                    if (field.name == expr.name) {
                        return field.type.kind == TypeKind::Reference ? std::optional<Type>(*field.type.pointee)
                                                                      : std::optional<Type>(field.type);
                    }
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        case ExprKind::Subscript: {
            std::optional<Type> base = infer_expr_type(*expr.lhs, body, signatures);
            if (!base) return std::nullopt;
            if (base->kind == TypeKind::Array) return *base->element;
            if (base->kind == TypeKind::Span) return *base->pointee;
            if (base->kind == TypeKind::Pointer) return *base->pointee;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] LocalState lookup(const StateMap& state, const std::string& name) {
    auto it = state.find(name);
    return it == state.end() ? LocalState::Bottom : it->second;
}

[[nodiscard]] bool binary_expr_has_compatible_types(const Expr& expr, const Body& body, const Signatures& signatures) {
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    if (!lhs_type.has_value() || !rhs_type.has_value()) return true;
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    if (types_equal(lhs_operand, rhs_operand)) return true;
    if (literal_compatible_with_type(*expr.lhs, rhs_operand) || literal_compatible_with_type(*expr.rhs, lhs_operand)) {
        return true;
    }
    return false;
}

void check_binary_expr_operand_types(const Expr& expr, const Body& body, const Signatures& signatures,
                                     const SourceLocation& loc) {
    if (expr.binary_op == BinaryOp::Assign) return;
    if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) return;
    if (expr.binary_op != BinaryOp::Eq && expr.binary_op != BinaryOp::Ne && expr.binary_op != BinaryOp::Lt &&
        expr.binary_op != BinaryOp::Gt && expr.binary_op != BinaryOp::Le && expr.binary_op != BinaryOp::Ge) {
        return;
    }
    if (binary_expr_has_compatible_types(expr, body, signatures)) return;
    std::optional<Type> lhs_type = infer_expr_type(*expr.lhs, body, signatures);
    std::optional<Type> rhs_type = infer_expr_type(*expr.rhs, body, signatures);
    if (!lhs_type.has_value() || !rhs_type.has_value()) return;
    const Type& lhs_operand = binary_operand_type(*lhs_type);
    const Type& rhs_operand = binary_operand_type(*rhs_type);
    throw DataflowError("binary operator requires operands of the same type; scpp has no implicit conversion between '" +
                            lhs_operand.name + "' and '" + rhs_operand.name + "' (ch06)",
                        loc);
}

// Resolves a `base.field` Member expression's own declared field type --
// `base` must be either a plain Identifier naming a struct/class-typed
// local or parameter (covers `this.field`, ch05 §5.12's rewritten
// captured-name access, as well as an ordinary `obj.field`) or the
// equivalent explicit `*this` spelling (`(*this).field`, ch05 §5.9).
// Anything else (a nested `a.b.c`, `arr[i].field`, ...) returns nullopt,
// left unsupported for now -- see DataflowState::class_field_types' own
// comment for why this lookup is possible at all despite movecheck's
// otherwise Body-only (no Program access) architecture.
[[nodiscard]] std::optional<Type> resolve_member_field_type(const Expr& member_expr, const Body& body,
                                                              const DataflowState& state) {
    if (member_expr.kind != ExprKind::Member) return std::nullopt;
    if (state.class_field_types == nullptr) return std::nullopt;
    std::string base_name;
    if (member_expr.lhs->kind == ExprKind::Identifier) {
        base_name = member_expr.lhs->name;
    } else if (is_explicit_star_this(*member_expr.lhs)) {
        base_name = "this";
    } else {
        return std::nullopt;
    }
    auto base_it = body.local_types.find(base_name);
    if (base_it == body.local_types.end()) return std::nullopt;
    const Type& base_type = base_it->second;
    const std::string& type_name = (base_type.kind == TypeKind::Reference ? *base_type.pointee : base_type).name;
    auto class_it = state.class_field_types->find(type_name);
    if (class_it == state.class_field_types->end()) return std::nullopt;
    auto field_it = class_it->second.find(member_expr.name);
    if (field_it == class_it->second.end()) return std::nullopt;
    return field_it->second;
}

// Validates that `operand` (a plain Identifier, e.g. `p`, or a
// `base.field` Member, e.g. `this.p` -- ch05 §5.12's rewritten
// captured-name access) currently names/resolves to a readable
// pointer-like value that `*p`/`p->x` (UnaryOp::Deref) is licensed to
// dereference at this stage: a raw pointer `T*` (only while
// `state.unsafe_depth > 0`, ch01 §1.3/ch02/ch05.5), a function pointer
// being parenthesized for a call (`(*fp)(...)`), or `*this`. Class
// overloads of `operator*` are rewritten to ordinary calls earlier in the
// pipeline, so they no longer reach this raw Deref validator. A
// `base.field` Member operand has no independent move/borrow-state of its
// own to check (movecheck tracks move/borrow state per plain local, not
// per struct/class field -- there is no way to move *out of* a field in
// this version at all, matching the documented pre-existing gap), so it
// is implicitly always considered "Initialized, unborrowed" -- only its
// *type* (and, for a raw pointer, the enclosing unsafe context) is
// checked.
void validate_deref_operand(const Expr& operand, const DataflowState& state, const Body& body,
                            const Signatures& signatures) {
    std::string describe = operand.kind == ExprKind::Member ? operand.lhs->name + "." + operand.name : operand.name;
    std::optional<Type> resolved =
        operand.kind == ExprKind::Member ? resolve_member_field_type(operand, body, state) : [&]() -> std::optional<Type> {
            auto it = body.local_types.find(operand.name);
            return it == body.local_types.end() ? std::nullopt : std::optional<Type>(it->second);
        }();
    const Type* underlying =
        resolved.has_value() && resolved->kind == TypeKind::Reference && resolved->pointee ? &*resolved->pointee
                                                                                            : (resolved ? &*resolved : nullptr);
    bool is_raw_ptr = resolved.has_value() && resolved->kind == TypeKind::Pointer;
    bool is_fn_ptr = resolved.has_value() && is_function_pointer(*resolved);
    bool is_class_deref =
        underlying != nullptr && underlying->kind == TypeKind::Named &&
        signatures.contains(underlying->name + "_operator_deref");
    bool is_this_ref = resolved.has_value() && operand.kind == ExprKind::Identifier && operand.name == "this" &&
                       resolved->kind == TypeKind::Reference;
    if (!is_raw_ptr && !is_fn_ptr && !is_class_deref && !is_this_ref) {
        throw DataflowError("cannot dereference ('*') '" + describe +
                             "': only a raw pointer (inside '[[scpp::unsafe]] { }'), a function pointer "
                             "being called, a class with operator*, or '*this' is supported here",
            state.current_loc);
    }
    if (is_raw_ptr && state.unsafe_depth == 0) {
        throw DataflowError("cannot dereference raw pointer '" + describe +
                             "': requires '[[scpp::unsafe]] { }' (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    if (operand.kind == ExprKind::Member) return; // no per-field move/borrow state -- see this function's own comment
    LocalState current = lookup(state.locals, operand.name);
    if (current != LocalState::Initialized) {
        throw DataflowError(describe_bad_state(operand.name, current),
            state.current_loc);
    }
}

// Handles a raw-pointer/function-pointer/`*this` Deref expression used as
// a plain read (not as a borrow source -- see resolve_borrow_source_root's
// own Deref case for that). Class overloads of `operator*` are rewritten
// to ordinary calls earlier in the pipeline, so they bypass this helper
// entirely. A raw pointer has no ownership/move state of its own to
// disturb. `*this` is likewise just an explicit spelling of the receiver
// object itself (ch05 §5.9), so it behaves exactly like reading `this`.
void apply_deref(const Expr& expr, const DataflowState& state, const Body& body, const Signatures& signatures,
                 bool report_errors) {
    if (is_explicit_star_this(expr)) {
        if (!report_errors) return;
        validate_deref_operand(*expr.lhs, state, body, signatures);
        return;
    }
    bool is_plain_identifier = expr.lhs->kind == ExprKind::Identifier;
    // ch05 §5.12: `*this.p`/`*p`, where a captured raw/function pointer was
    // rewritten to a `this.p` Member access by the closure's own
    // field-access rewrite (rewrite_captured_identifiers_as_field_access)
    // -- see validate_deref_operand's own comment for why a Member operand
    // has no separate move/borrow state to check beyond its type.
    bool is_member_of_identifier =
        expr.lhs->kind == ExprKind::Member &&
        (expr.lhs->lhs->kind == ExprKind::Identifier || is_explicit_star_this(*expr.lhs->lhs));
    if (!is_plain_identifier && !is_member_of_identifier) {
        if (report_errors) {
            throw DataflowError("dereference ('*') currently only supports a plain local raw/function pointer "
                                 "variable, '*this', or a captured field of one ('this.field') (not a subscript "
                                 "or other expression)",
                state.current_loc);
        }
        return;
    }
    if (!report_errors) return; // purely diagnostic: doesn't move p or change any tracked state
    validate_deref_operand(*expr.lhs, state, body, signatures);
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

void release_closure_capture_borrows(const std::string& name, DataflowState& state) {
    auto closure_it = state.closure_capture_borrows.find(name);
    if (closure_it == state.closure_capture_borrows.end()) return;
    for (const ClosureCaptureBorrow& capture_borrow : closure_it->second) {
        auto borrow_it = state.borrows.find(capture_borrow.root);
        if (borrow_it == state.borrows.end()) continue;
        if (capture_borrow.is_mutable) {
            borrow_it->second.mutable_borrow = false;
        } else if (borrow_it->second.shared_count > 0) {
            borrow_it->second.shared_count--;
        }
        if (!borrow_it->second.mutable_borrow && borrow_it->second.shared_count == 0) {
            state.borrows.erase(borrow_it);
        }
    }
    state.closure_capture_borrows.erase(closure_it);
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
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
            return;
        case ExprKind::New:
            for (const auto& arg : expr->args) collect_reference_uses(arg.get(), body, out);
            return;
        case ExprKind::Delete:
        case ExprKind::PackExpansion:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Identifier: {
            auto it = body.local_types.find(expr->name);
            if (it != body.local_types.end() &&
                (is_reference(it->second) || is_span(it->second) ||
                 body.borrow_holding_closure_locals.contains(expr->name))) {
                out.insert(expr->name);
            }
            return;
        }
        case ExprKind::Binary:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            return;
        case ExprKind::Conditional:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
            collect_reference_uses(expr->third.get(), body, out);
            return;
        case ExprKind::Unary:
        case ExprKind::Move:
        case ExprKind::Cast:
            collect_reference_uses(expr->lhs.get(), body, out);
            return;
        case ExprKind::Call:
            if (expr->lhs != nullptr) {
                collect_reference_uses(expr->lhs.get(), body, out);
            } else {
                auto it = body.local_types.find(expr->name);
                if (it != body.local_types.end() &&
                    (is_reference(it->second) || is_span(it->second) ||
                     body.borrow_holding_closure_locals.contains(expr->name))) {
                    out.insert(expr->name);
                }
            }
            for (const auto& arg : expr->args) {
                collect_reference_uses(arg.get(), body, out);
            }
            return;
        case ExprKind::Fold:
            collect_reference_uses(expr->lhs.get(), body, out);
            collect_reference_uses(expr->rhs.get(), body, out);
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
    dead.clear();
    for (const auto& [name, borrows] : state.closure_capture_borrows) {
        if (!live_after_stmt.contains(name)) dead.push_back(name);
    }
    for (const std::string& name : dead) {
        release_closure_capture_borrows(name, state);
    }
}

void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
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
            apply_expr(*expr.rhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);

        case ExprKind::Unary: {
            // `*p`/`p->x` (a raw pointer local here, or a class
            // `operator*` call rewritten elsewhere; see
            // validate_deref_operand): the root is `p` itself, *not* p's
            // pointee, so that moving or reassigning `p` while a
            // reference into `*p` is alive is rejected by the exact same
            // borrow-conflict checks that already guard every other root
            // (apply_statement's Assign case, and the Move case's own
            // borrow check above) -- freeing or reassigning p's
            // allocation out from under a live reference would otherwise
            // be a use-after-free.
            if (is_explicit_star_this(expr)) return "this";
            if (expr.unary_op != UnaryOp::Deref) {
                if (report_errors) {
                    throw DataflowError("a reference can currently only borrow a plain local variable, a "
                                         "field of one ('a.b'), an array element of one ('arr[i]'), a "
                                         "dereferenced raw-pointer local ('*p'/'p->x'), or "
                                         "the result of a call to a reference-returning function -- not an "
                                         "arbitrary expression",
                        state.current_loc);
                }
                return "";
            }
            if (report_errors) validate_deref_operand(*expr.lhs, state, body, signatures);
            if (expr.lhs->kind == ExprKind::Identifier) return resolve_root_place(expr.lhs->name, state);
            if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                return resolve_borrow_source_root(*expr.lhs->lhs, state, body, signatures, report_errors);
            }
            return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
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
            if (expr.name == "operator_deref" && expr.lhs != nullptr && elided_index < callee.param_offset) {
                if (expr.lhs->kind == ExprKind::Identifier) return resolve_root_place(expr.lhs->name, state);
                if (expr.lhs->kind == ExprKind::Member && expr.lhs->lhs) {
                    return resolve_borrow_source_root(*expr.lhs->lhs, state, body, signatures, report_errors);
                }
                return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
            }
            if (elided_index < callee.param_offset) {
                // ch04 §4.2/ch05 §5.9: the elided parameter is the
                // method's own implicit receiver (`this`, always
                // params[0] when present -- see make_this_param and
                // resolve_elided_param_index's own this-elision special
                // case) -- resolved through `expr.lhs` (the receiver
                // expression), never `expr.args` (which has no entry for
                // the receiver at all -- see resolve_callee_signature).
                // A real, discovered-and-fixed bug: indexing `expr.args`
                // directly with an elided index that actually refers to
                // `this` (e.g. any reference-returning method taking no
                // *other* reference-compatible parameter, such as a
                // plain getter `int& get() { return this.v; }`, or the
                // copy-assignment-operator shape this feature adds)
                // previously read out of bounds whenever the call had
                // fewer explicit arguments than the elided index alone
                // would suggest.
                return resolve_borrow_source_root(*expr.lhs, state, body, signatures, report_errors);
            }
            // The elided parameter's *own* argument is what the call's
            // result transitively borrows from -- resolve it the exact
            // same way as any other borrow source (recursively, so a
            // chain of reference-returning calls is followed all the
            // way back to a real place).
            return resolve_borrow_source_root(*expr.args[elided_index - callee.param_offset], state, body,
                                               signatures, report_errors);
        }

        default:
            if (report_errors) {
                throw DataflowError("a reference can currently only borrow a plain local variable, a field of "
                                     "one ('a.b'), an array element of one ('arr[i]'), a dereferenced "
                                     "raw-pointer local ('*p'/'p->x'), or the result of a call "
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
            // Class `operator*` rewrites that yield a read-only view are
            // handled by the Call case just below, via the method's own
            // declared return type.
            if (is_explicit_star_this(expr)) return is_read_only_reachable(*expr.lhs, body, signatures);
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
    if (expr.lhs->kind == ExprKind::Identifier && !body.local_types.contains(expr.lhs->name) &&
        signatures.contains(expr.lhs->name)) {
        return;
    }
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
    // ch05 §5.x: a *const* reference parameter bound directly to a fresh
    // rvalue argument (a literal, std::move/std::make_unique, a lambda
    // literal, or a call not itself returning by reference) binds to a
    // freshly-materialized temporary -- exactly like real C++'s own
    // temporary lifetime extension (mirrors argument_matches_parameter's
    // identical acceptance of this shape during overload resolution).
    // Never reached for a *mutable* `T&` (real C++ itself forbids binding
    // a non-const lvalue reference to a temporary). A fresh temporary
    // aliases nothing else in the entire program, so there is nothing
    // further to check here at all: just evaluate `arg` for its own side
    // effects (e.g. std::move's move-out bookkeeping) and return, skipping
    // resolve_borrow_source_root/every borrow-conflict check below
    // entirely (there is no "root" at all for a temporary).
    if (!param_type.is_mutable_ref && produces_rvalue_of_type(arg, *param_type.pointee, body, signatures)) {
        apply_expr(arg, /*is_move_target_context=*/false, state, body, signatures, report_errors);
        return;
    }

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
    // A method call's receiver (`obj.method(...)`/`this->method(...)`,
    // stored in `expr.lhs`, never part of `expr.args` -- see
    // CalleeSignature's own comment) is an ordinary read of `obj` and was
    // previously never visited at all here, unlike the identical
    // receiver sub-expression on a field access (ExprKind::Member's own
    // `apply_expr(*expr.lhs, ...)` call) -- a real, discovered-and-fixed
    // gap: calling *any* method (a mutating one or a read-only `const`
    // getter alike) on a moved-out class-typed variable went entirely
    // unchecked, even though reading one of its fields directly was
    // already correctly rejected. Also covers an IIFE's lambda literal
    // receiver (`[capture](args){...}(...)`, ExprKind::Lambda -- see
    // resolve_callee_signature's own comment): this is also the only
    // place that would otherwise ever visit that literal at all when it
    // is called immediately rather than bound to a variable first, so
    // this fix incidentally makes an IIFE's own captures subject to the
    // same checking (apply_expr's Lambda case, which calls
    // apply_lambda_captures) that a stored closure already got via the
    // VarDecl case in apply_statement -- previously entirely unchecked
    // too (e.g. an IIFE could init-capture an already-moved-out
    // std::unique_ptr without error).
    std::optional<Type> direct_call_type = expr.lhs == nullptr ? infer_expr_type(expr, body, signatures) : std::nullopt;
    if (!expr.lhs && direct_call_type.has_value() && direct_call_type->kind == TypeKind::Named &&
        state.class_names != nullptr && state.class_names->contains(expr.name)) {
        check_constructor_arguments(direct_call_type->name, expr.args, state, body, signatures, report_errors);
        return;
    }
    CalleeSignature callee = resolve_callee_signature(expr, body, state.class_field_types);
    auto name_it = signatures.find(callee.key);
    const FunctionSignature* sig = resolve_overload(expr, callee, body, signatures);
    if (expr.lhs) {
        bool receiver_is_reference =
            sig != nullptr && !sig->param_types.empty() && is_reference(sig->param_types[0]) && !sig->param_types[0].is_rvalue_ref;
        (void)receiver_is_reference;
        apply_expr(*expr.lhs, /*is_move_target_context=*/expr.lhs->kind == ExprKind::Move, state, body, signatures,
                   report_errors);
    }
    std::string callee_display = expr.name;
    if (callee_display.empty()) {
        if (expr.lhs && expr.lhs->kind == ExprKind::Identifier) {
            callee_display = expr.lhs->name;
        } else {
            callee_display = "<function pointer>";
        }
    }
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
    if (report_errors && sig != nullptr && sig->is_extern_c_declaration_only && state.unsafe_depth == 0) {
        throw DataflowError("cannot call 'extern \"C\"' function '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': no scpp compiler ever sees its real "
                             "implementation to check it (spec ch01 §1.3/ch02)",
            state.current_loc);
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        throw DataflowError("cannot call '" + callee_display +
                             "' outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
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
            apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
        } else if (param_is_reference) {
            apply_reference_argument(arg, sig->param_types[param_index], state, in_call_borrows, body, signatures,
                                      report_errors);
        } else {
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() && is_named_class_type(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body);
            if (report_errors && class_value_param && !copyable_lvalue_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures)) {
                throw DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either a copyable bare local of that exact type or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/!copyable_lvalue_source, state, body, signatures, report_errors);

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

// ch04 §4.2: checks every argument of a `ClassName name(args);`
// constructor-call VarDecl (Stmt::ctor_args) -- mirrors
// check_call_arguments' own per-argument dataflow processing/validation
// (reference-argument borrowing, rvalue-reference genuine-rvalue
// requirement, unsafe-constructor gating) exactly, just resolved against
// `class_name + "_new"`'s own signature(s) instead of an ordinary Call
// expression's callee, since a constructor-call VarDecl has no wrapping
// Call Expr of its own to hand to resolve_callee_signature/resolve_
// overload (Stmt::ctor_args is a bare argument list). `param_offset` is
// always 1 (the implicit `this`, exactly like any other method --
// see make_this_param), and the receiver is unconditionally treated as
// fully mutable (a freshly-constructed object always accepts a mutable
// `this` -- there's no *existing* object yet for read-only-reachability
// to apply to, mirroring codegen's own resolve_overload_by_type default).
// Previously, constructor arguments were entirely invisible to the
// dataflow checker (a has_ctor_args VarDecl lowered to a bare,
// argument-blind Declare, see mir.cppm) -- e.g. `Holder h(std::move(p));`
// never marked `p` moved-out at all. Multiple candidates matching by
// argument count alone (ambiguous, or none at all) leave `sig` null,
// exactly like resolve_overload's own "let a more specific, later check
// report it" pattern -- codegen's own resolve_overload_by_type
// independently re-derives the same answer and is the one that actually
// rejects an unresolvable call.
void check_constructor_arguments(const std::string& class_name, const std::vector<ExprPtr>& ctor_args,
                                  DataflowState& state, const Body& body, const Signatures& signatures,
                                  bool report_errors) {
    std::string ctor_name = class_name + "_new";
    const FunctionSignature* sig = nullptr;
    auto name_it = signatures.find(ctor_name);
    if (name_it != signatures.end()) {
        const std::vector<FunctionSignature>& candidates = name_it->second;
        if (candidates.size() == 1) {
            sig = &candidates[0];
        } else {
            std::vector<const FunctionSignature*> matches;
            for (const FunctionSignature& candidate : candidates) {
                if (candidate.param_types.size() != ctor_args.size() + 1) continue;
                bool all_match = true;
                for (size_t i = 0; all_match && i < ctor_args.size(); i++) {
                    all_match =
                        argument_matches_parameter(*ctor_args[i], candidate.param_types[i + 1], body, signatures);
                }
                if (all_match) matches.push_back(&candidate);
            }
            if (matches.size() == 1) sig = matches[0];
        }
    }
    if (report_errors && sig != nullptr && sig->is_unsafe && state.unsafe_depth == 0) {
        throw DataflowError("cannot call '" + class_name +
                             "'s constructor outside '[[scpp::unsafe]] { }': its own declaration is marked "
                             "'[[scpp::unsafe]]', so its soundness depends on a precondition only the "
                             "caller can guarantee (ch01 §1.2/§1.3)",
            state.current_loc);
    }
    BorrowMap in_call_borrows;
    for (size_t i = 0; i < ctor_args.size(); i++) {
        const Expr& arg = *ctor_args[i];
        size_t param_index = i + 1;
        bool param_is_reference =
            sig != nullptr && param_index < sig->param_types.size() && is_reference(sig->param_types[param_index]);
        bool param_is_rvalue_reference = param_is_reference && sig->param_types[param_index].is_rvalue_ref;
        if (param_is_rvalue_reference) {
            if (report_errors &&
                !produces_rvalue_of_type(arg, *sig->param_types[param_index].pointee, body, signatures)) {
                throw DataflowError(
                    "argument to an rvalue-reference ('T&&') parameter must be a fresh value -- "
                    "std::move(x), std::make_unique<T>(...), a literal, or a call returning by value; "
                    "an existing named variable must be moved explicitly (spec ch03/ch05 §5.11)",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/true, state, body, signatures, report_errors);
        } else if (param_is_reference) {
            apply_reference_argument(arg, sig->param_types[param_index], state, in_call_borrows, body, signatures,
                                      report_errors);
        } else {
            bool class_value_param =
                sig != nullptr && param_index < sig->param_types.size() && is_named_class_type(sig->param_types[param_index], body);
            bool copyable_lvalue_source =
                class_value_param && is_copyable_class_lvalue_boundary_source(arg, sig->param_types[param_index], body);
            if (report_errors && class_value_param && !copyable_lvalue_source &&
                !produces_rvalue_of_type(arg, sig->param_types[param_index], body, signatures)) {
                throw DataflowError("passing class '" + sig->param_types[param_index].name +
                                     "' by value requires either a copyable bare local of that exact type or "
                                     "a fresh value such as std::move(x) or a call returning by value",
                    state.current_loc);
            }
            apply_expr(arg, /*is_move_target_context=*/!copyable_lvalue_source, state, body, signatures, report_errors);
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
//  - a by-value capture of a class type uses the same copy/move boundary
//    rules as any other class value construction site in the language:
//    a bare same-type lvalue copies only if the class is
//    copy-constructible, otherwise the source must be an rvalue (e.g.
//    `std::move(p)` in an init-capture).
//  - a by-reference capture is checked exactly like a reference-typed
//    call argument (apply_reference_argument): the closure's own field
//    genuinely borrows it, for as long as `reference_capture_borrows`
//    (see above) says it lasts.
void apply_lambda_captures(const Expr& expr, DataflowState& state, BorrowMap& reference_capture_borrows,
                            const Body& body, const Signatures& signatures, bool report_errors,
                            std::vector<ClosureCaptureBorrow>* out_closure_capture_borrows = nullptr) {
    auto apply_by_value_capture_source = [&](const Expr& source, const Type& source_type,
                                             const std::string& capture_display) {
        if (is_named_class_type(source_type, body)) {
            bool is_copy_source = is_bare_same_type_copy_source(source, source_type, body);
            bool is_rvalue_source = produces_rvalue_of_type(source, source_type, body, signatures);
            if (report_errors) {
                if (is_copy_source) {
                    if (state.classes_with_copy_ctor == nullptr ||
                        !state.classes_with_copy_ctor->contains(source_type.name)) {
                        throw DataflowError(
                            "capture '" + capture_display + "' of class '" + source_type.name +
                               "' requires std::move or another rvalue source because the class is not "
                               "copy-constructible (spec §6.5/§5.12)",
                            state.current_loc);
                    }
                } else if (!is_rvalue_source) {
                    throw DataflowError(
                        "capture '" + capture_display + "' of class '" + source_type.name +
                            "' must use a plain same-typed variable (if copy-constructible) or an rvalue such as "
                            "std::move(...) (spec §6.5/§5.12)",
                        state.current_loc);
                }
            }
            apply_expr(source, /*is_move_target_context=*/is_rvalue_source, state, body, signatures, report_errors);
            return;
        }
        apply_expr(source, /*is_move_target_context=*/source.kind == ExprKind::Move, state, body, signatures,
                   report_errors);
    };
    for (const LambdaCapture& capture : expr.lambda_captures) {
        if (capture.init) {
            std::optional<Type> init_type = infer_expr_type(*capture.init, body, signatures);
            if (init_type.has_value()) {
                apply_by_value_capture_source(*capture.init, *init_type, capture.name);
            } else {
                apply_expr(*capture.init, /*is_move_target_context=*/true, state, body, signatures, report_errors);
            }
            continue;
        }
        if (!capture.by_reference) {
            auto type_it = body.local_types.find(capture.name);
            if (type_it != body.local_types.end()) {
                Expr capture_ident;
                capture_ident.kind = ExprKind::Identifier;
                capture_ident.loc = expr.loc;
                capture_ident.name = capture.name;
                apply_by_value_capture_source(capture_ident, type_it->second, capture.name);
                continue;
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
        std::string root =
            resolve_borrow_source_root(capture_ident, state, body, signatures, report_errors);
        Type ref_type;
        ref_type.kind = TypeKind::Reference;
        ref_type.is_mutable_ref = true; // matches resolve_lambda's own field choice
        apply_reference_argument(capture_ident, ref_type, state, reference_capture_borrows, body, signatures,
                                  report_errors);
        if (out_closure_capture_borrows != nullptr) {
            out_closure_capture_borrows->push_back(ClosureCaptureBorrow{root, true});
        }
    }
}

// Walks `expr`, updating `state` for any std::move / assignment / borrow
// side effects and, when `report_errors` is true, throwing DataflowError
// on an unsafe read. `is_move_target_context` is true exactly where
// a bare `std::move(x)` is allowed to appear: a var-decl initializer, an
// assignment RHS, a return value, a call argument, a constructor-call
// argument, or a by-value class lambda capture initializer (ch04 §4.2 --
// see check_constructor_arguments and apply_lambda_captures). ch04
// §4.2/ch05 §5.15/spec §6.4: `std::move(x)` is legitimate here for any
// class-typed variable -- move construction/assignment for `class` types
// is always the compiler-provided memberwise operation (never
// user-written, spec §6.4(1)), so there is no additional per-class
// validation to do here beyond the ordinary move-state bookkeeping every
// movable type already gets.
//
// This function is run twice per program point: once during the
// worklist's fixed-point iteration (report_errors=false, just to compute
// stable per-block states) and once more in the final reporting pass
// (report_errors=true). Both runs must apply the *same* state mutations so
// the two phases stay consistent.
void apply_expr(const Expr& expr, bool is_move_target_context, DataflowState& state, const Body& body,
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
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
            return;

        case ExprKind::Identifier: {
            if (!report_errors) return;
            auto type_it = body.local_types.find(expr.name);
            if (type_it == body.local_types.end()) return; // unknown name: left to codegen's own check
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
            // ch04 §4.2/spec §6.4: any class type (move
            // construction/assignment, always the compiler-provided
            // memberwise operation -- never a struct/scalar, which isn't
            // move-restricted at all (always freely copyable). Also
            // recognizes an rvalue-reference-*to*-class
            // local/parameter (`Inner&& i`, ch03/ch05 §5.11): `i` itself
            // is a name, like any other, that can appear as `std::move`'s
            // own operand (mirrors real C++: a *named* rvalue reference
            // is itself an lvalue, so moving out of what it refers to
            // still needs an explicit std::move) -- moving out of `i`
            // here moves out of *its own current referent* (whatever
            // temporary/argument that rvalue-reference parameter is
            // itself bound to), not `i`'s own (nonexistent, references
            // are never owning) storage.
            auto is_named_class = [&](const Type& t) {
                return t.kind == TypeKind::Named && state.class_names != nullptr && state.class_names->contains(t.name);
            };
            bool is_movable_class =
                type_it != body.local_types.end() &&
                (is_named_class(type_it->second) ||
                 (type_it->second.kind == TypeKind::Reference && type_it->second.pointee &&
                  is_named_class(*type_it->second.pointee)));
            if (type_it == body.local_types.end() || !is_movable_class) {
                if (report_errors) {
                    throw DataflowError("std::move is only supported for class-typed variables in this version; '" +
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
            if (report_errors && !is_move_target_context) {
                throw DataflowError("std::move(" + name + ") must be used to initialize, assign into, return, "
                                                            "pass, or capture a same-typed class value",
                    state.current_loc);
            }
            return;
        }

        // `static_cast<T>(expr)`/`(T)expr` (ch06 §6): visits the operand
        // for its own move/borrow bookkeeping exactly like any other
        // sub-expression (never itself a move-target-context -- a cast
        // reads its operand's value, it doesn't take ownership of it),
        // then validates the (source, target) pair is actually a legal
        // conversion in this version: scalar-to-scalar (always) or
        // raw-pointer-to-raw-pointer only inside an unsafe context
        // (spec §5.1(5.2)).
        case ExprKind::Cast: {
            apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (report_errors) {
                std::optional<Type> source_type = infer_expr_type(*expr.lhs, body, signatures);
                bool scalar_source = source_type.has_value() && source_type->kind == TypeKind::Named &&
                                     is_scalar_type_name(source_type->name);
                bool scalar_target = expr.type.kind == TypeKind::Named && is_scalar_type_name(expr.type.name);
                if (scalar_source && scalar_target) return;

                bool raw_pointer_source = source_type.has_value() && source_type->kind == TypeKind::Pointer;
                bool raw_pointer_target = expr.type.kind == TypeKind::Pointer;
                if (raw_pointer_source && raw_pointer_target) {
                    if (state.unsafe_depth == 0) {
                        throw DataflowError("cannot cast between raw pointer types outside '[[scpp::unsafe]] { }' "
                                                "(spec §5.1(5.2))",
                                            state.current_loc);
                    }
                    return;
                }

                {
                    throw DataflowError(
                        "a cast is only supported between two scalar types, or between two raw pointer types "
                        "inside '[[scpp::unsafe]] { }', in this version",
                        state.current_loc);
                }
            }
            return;
        }

        case ExprKind::Unary:
            if (expr.unary_op == UnaryOp::Deref) {
                apply_deref(expr, state, body, signatures, report_errors);
                return;
            }
            if (expr.unary_op == UnaryOp::AddressOf) {
                apply_address_of(expr, state, body, signatures, report_errors);
                return;
            }
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::New:
            if (report_errors && state.unsafe_depth == 0) {
                throw DataflowError("cannot use 'new' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc);
            }
            if (expr.type.kind == TypeKind::Named && state.class_names != nullptr && state.class_names->contains(expr.type.name)) {
                bool move_shape = expr.args.size() == 1 && expr.args[0]->kind == ExprKind::Move &&
                                  produces_rvalue_of_type(*expr.args[0], expr.type, body, signatures);
                if (move_shape) {
                    apply_expr(*expr.args[0], /*is_move_target_context=*/true, state, body, signatures, report_errors);
                } else if (expr.args.size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(expr.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*expr.args[0], expr.type, body)) {
                    apply_expr(*expr.args[0], /*is_move_target_context=*/false, state, body, signatures, report_errors);
                } else {
                    check_constructor_arguments(expr.type.name, expr.args, state, body, signatures, report_errors);
                }
                return;
            }
            for (const auto& arg : expr.args) {
                apply_expr(*arg, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            }
            return;

        case ExprKind::Delete:
            apply_expr(*expr.lhs, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (report_errors && state.unsafe_depth == 0) {
                throw DataflowError("cannot use 'delete' outside '[[scpp::unsafe]] { }' (spec §5.1(5.4))",
                                    state.current_loc);
            }
            if (report_errors) {
                std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!operand_type.has_value() || operand_type->kind != TypeKind::Pointer) {
                    throw DataflowError("'delete' requires a raw pointer operand in this version",
                                        state.current_loc);
                }
            }
            return;

        case ExprKind::Fold:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            if (expr.rhs) apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::Conditional:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.third, false, state, body, signatures, report_errors);
            if (report_errors) {
                std::optional<Type> condition_type = infer_expr_type(*expr.lhs, body, signatures);
                if (!condition_type.has_value() || condition_type->kind != TypeKind::Named ||
                    condition_type->name != "bool") {
                    throw DataflowError("conditional operator requires a 'bool' condition", state.current_loc);
                }
                std::optional<Type> then_type = infer_expr_type(*expr.rhs, body, signatures);
                std::optional<Type> else_type = infer_expr_type(*expr.third, body, signatures);
                if (then_type.has_value() && else_type.has_value() && !types_equal(*then_type, *else_type)) {
                    throw DataflowError("conditional operator requires both arms to have the same type",
                                        state.current_loc);
                }
            }
            return;

        case ExprKind::Binary:
            if (expr.binary_op == BinaryOp::Assign) {
                bool target_is_movable_class = false;
                std::optional<Type> target_class_type;
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    if (it != body.local_types.end() && it->second.kind == TypeKind::Named &&
                        state.class_names != nullptr && state.class_names->contains(it->second.name)) {
                        target_is_movable_class = true;
                        target_class_type = it->second;
                    }
                } else if (expr.lhs->kind == ExprKind::Member) {
                    // ch04 §4.2/spec §6.4/§6.5: `this.field = std::move(x);`
                    // (or any `obj.field = std::move(x);`) -- a field
                    // write is always "reinitializing" regardless of
                    // prior state (see this case's own Member branch
                    // below: struct/class locals are Initialized as a
                    // whole from declaration, so a field write never
                    // needs its own separate reassignment gate the way a
                    // *whole* class-typed local does) -- the only thing
                    // worth resolving here is whether std::move is
                    // *licensed* at all for this field's own declared
                    // type, exactly like the Identifier case just above.
                    // Recognizes any class-typed field, so a constructor
                    // moving a by-value/by-move parameter directly into
                    // its own field (e.g. `Outer(Inner&& i) { this.inner =
                    // std::move(i); }`) works the same way as any other
                    // class move.
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state);
                    if (field_type.has_value() && field_type->kind == TypeKind::Named &&
                        state.class_names != nullptr && state.class_names->contains(field_type->name)) {
                        target_is_movable_class = true;
                        target_class_type = field_type;
                    }
                }
                // spec §6.5: a bare (non-move) same-type Identifier RHS
                // assigned into a class-typed *field* target is copy
                // assignment -- licensed only when the class is copy-
                // assignable. This is the only gate a field-target copy
                // assignment gets at all (a field write is never subject
                // to the whole-local-only "first write vs. reassignment"
                // restriction the MirStatementKind::Assign case enforces
                // for a plain local target, which is also where a
                // *whole-local* copy assignment's own, separate
                // eligibility check already lives -- a whole-local
                // Assign statement lowers directly to that MIR node, and
                // never reaches this general expression-level handler at
                // all, so there is no duplicate/conflicting check here
                // for that case; this exists for the Member-target case
                // specifically, previously a real, unchecked gap -- see
                // is_bare_same_type_copy_source's own comment).
                if (report_errors && target_class_type.has_value() &&
                    is_bare_same_type_copy_source(*expr.rhs, *target_class_type, body) &&
                    (state.classes_with_copy_assign == nullptr ||
                     !state.classes_with_copy_assign->contains(target_class_type->name))) {
                    throw DataflowError("class '" + target_class_type->name +
                                         "' is not copy-assignable (spec §6.5(3)) -- this assignment is not "
                                         "licensed",
                        state.current_loc);
                }
                bool is_move_target = target_is_movable_class;
                apply_expr(*expr.rhs, is_move_target, state, body, signatures, report_errors);
                if (expr.lhs->kind == ExprKind::Identifier) {
                    auto it = body.local_types.find(expr.lhs->name);
                    if (it != body.local_types.end()) {
                        check_function_pointer_assignment(it->second, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                    }
                } else if (expr.lhs->kind == ExprKind::Member) {
                    std::optional<Type> field_type = resolve_member_field_type(*expr.lhs, body, state);
                    if (field_type.has_value()) {
                        check_function_pointer_assignment(*field_type, *expr.rhs, body, signatures, state.current_loc,
                                                          expr.lhs->name, report_errors);
                    }
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
            if (report_errors) check_binary_expr_operand_types(expr, body, signatures, state.current_loc);
            return;

        case ExprKind::Call:
            check_call_arguments(expr, state, body, signatures, report_errors);
            return;

        case ExprKind::Member: {
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            // ch04 §4.2: real, unrestricted C++ access control -- a
            // member variable may be `public` or `private` in any
            // combination. External access (from outside the class's
            // own methods) to a `private` field is rejected, exactly
            // like before; a `public` one is now allowed (checked
            // exactly like a struct field access -- the borrow itself
            // is still recorded against the whole root object,
            // conservatively, by the caller's own apply_place/
            // resolve_borrow_source_root machinery, unaffected by this
            // access-control gate). Scoped to a plain Identifier base
            // (`this`, or an ordinary local/parameter) for now --
            // movecheck doesn't otherwise infer the type of an arbitrary
            // nested expression, so a deeper chain (`a.b.field` where
            // `a.b` is itself class-typed) isn't covered by this check
            // yet, a known, narrow scope limitation.
            if (report_errors && expr.lhs->kind == ExprKind::Identifier && state.class_names != nullptr) {
                auto type_it = body.local_types.find(expr.lhs->name);
                if (type_it != body.local_types.end()) {
                    std::string class_name = named_type_name(type_it->second);
                    if (!class_name.empty() && state.class_names->contains(class_name) &&
                        class_name != state.current_class) {
                        AccessSpecifier access = AccessSpecifier::Private;
                        if (state.class_field_access != nullptr) {
                            auto class_it = state.class_field_access->find(class_name);
                            if (class_it != state.class_field_access->end()) {
                                auto field_it = class_it->second.find(expr.name);
                                if (field_it != class_it->second.end()) access = field_it->second;
                            }
                        }
                        if (access == AccessSpecifier::Private) {
                            throw DataflowError("cannot access private member '" + expr.name + "' of class '" +
                                                 class_name + "' from outside its own methods (ch04 §4.2)",
                                state.current_loc);
                        }
                    }
                }
            }
            return;
        }

        case ExprKind::Subscript:
            apply_expr(*expr.lhs, false, state, body, signatures, report_errors);
            apply_expr(*expr.rhs, false, state, body, signatures, report_errors);
            return;

        case ExprKind::PackExpansion:
            if (report_errors) {
                throw DataflowError("unexpanded parameter-pack expression reached move checking",
                                    state.current_loc);
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

    // ch05 §5.x: `const T& r = <rvalue>;` (a literal, std::move/
    // std::make_unique, a lambda literal, or a call not itself returning
    // by reference) binds to a freshly-materialized temporary -- exactly
    // like real C++'s own temporary lifetime extension (mirrors
    // apply_reference_argument's identical handling for a call
    // argument). Scoped to `TypeKind::Reference` only (never `Span`: a
    // std::span is only ever constructed from an existing fixed-size
    // array, ch06, never a fresh rvalue) and to a *const* reference (real
    // C++ forbids binding a *mutable* lvalue reference to a temporary). A
    // fresh temporary aliases nothing else in the program, so there is
    // no "root" to track in state.borrows/state.ref_targets at all --
    // just evaluate the initializer for its own side effects and mark
    // `stmt.local` initialized.
    if (stmt.type.kind == TypeKind::Reference && !stmt.type.is_mutable_ref &&
        produces_rvalue_of_type(*stmt.expr, *stmt.type.pointee, body, signatures)) {
        apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
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
    apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
}

// ch04 §4.2/spec §6.4: true exactly when `ctor_args` is the single-
// argument shape `std::move(x)` where `x`'s own declared type is the
// exact same class `constructed_type` names -- the shape that dispatches
// to the compiler-synthesized move constructor (spec §6.4(2)) rather
// than any of the class's own user-declared constructors (which can
// never themselves be a move constructor -- spec §6.4(1) forbids
// declaring one, enforced at parse time). A mismatched-type std::move
// argument (or any other shape) falls through to ordinary constructor
// resolution unchanged, exactly as it always has -- e.g. a real,
// user-declared `Bar(Foo&& f)` constructor taking a *different* type's
// rvalue reference is untouched by this and still resolved by
// check_constructor_arguments below.
[[nodiscard]] bool is_move_construction_shape(const std::vector<ExprPtr>& ctor_args, const Type& constructed_type,
                                               const Body& body) {
    if (ctor_args.size() != 1) return false;
    const Expr& arg = *ctor_args[0];
    if (arg.kind != ExprKind::Move || arg.lhs->kind != ExprKind::Identifier) return false;
    auto type_it = body.local_types.find(arg.lhs->name);
    return type_it != body.local_types.end() && types_equal(type_it->second, constructed_type);
}

// spec §6.5: true exactly when `expr` is a bare (non-move) plain
// variable of the exact same class type as `target_type` -- the shape
// spec §6.5's own worked example uses for both copy construction
// (`Foo b = a;`) and copy assignment (`b = a;`). Scoped to exactly this
// one shape (a plain Identifier, not a Member/Subscript/Call chain) --
// the same narrow, deliberate scoping is_move_construction_shape above
// uses for its own single-argument std::move recognition.
[[nodiscard]] bool is_bare_same_type_copy_source(const Expr& expr, const Type& target_type, const Body& body) {
    if (expr.kind != ExprKind::Identifier) return false;
    auto type_it = body.local_types.find(expr.name);
    if (type_it == body.local_types.end()) return false;
    if (types_equal(type_it->second, target_type)) return true;
    return type_it->second.kind == TypeKind::Reference && !type_it->second.is_rvalue_ref && type_it->second.pointee &&
           types_equal(*type_it->second.pointee, target_type);
}

void apply_statement(const MirStatement& stmt, DataflowState& state, const Body& body, const Signatures& signatures,
                      bool report_errors) {
    // See apply_expr's identical opening comment -- same reasoning, one
    // level up (statement rather than expression granularity).
    state.current_loc = stmt.loc;
    switch (stmt.kind) {
        case MirStatementKind::Declare:
            // ch04 §4.2: a constructor-call VarDecl (`ClassName name
            // (args);`) needs its own arguments' move/borrow effects
            // applied -- see MirStatement::ctor_args' own comment for
            // why this was previously entirely invisible here. A
            // std::move(x)-of-the-same-class single argument dispatches
            // to the compiler-synthesized move constructor directly
            // (spec §6.4(2)); anything else goes through ordinary
            // constructor-overload argument checking.
            if (stmt.ctor_args != nullptr) {
                if (is_move_construction_shape(*stmt.ctor_args, stmt.type, body)) {
                    apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/true, state, body, signatures,
                               report_errors);
                } else if (stmt.ctor_args->size() == 1 &&
                           body.program != nullptr && !has_user_declared_copy_ctor(stmt.type.name, *body.program) &&
                           is_copyable_class_lvalue_boundary_source(*(*stmt.ctor_args)[0], stmt.type, body)) {
                    apply_expr(*(*stmt.ctor_args)[0], /*is_move_target_context=*/false, state, body, signatures,
                               report_errors);
                } else {
                    check_constructor_arguments(stmt.type.name, *stmt.ctor_args, state, body, signatures,
                                                 report_errors);
                }
            }
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
            // ch05/ch06: a `const`-qualified local (Stmt::is_const,
            // Body::const_locals) is initialized exactly once, by the
            // very same Assign statement its own VarDecl lowers to (see
            // mir.cppm's VarDecl case) -- distinguished from a genuine
            // later reassignment attempt by whether `stmt.local` already
            // has a prior entry in `state.locals` at all, the identical
            // "first write vs. reassignment" test the class-typed-local
            // case below uses for its own, differently-motivated
            // restriction. Checked *before* every type-specific case
            // below (reference/span/class/unique_ptr/plain scalar) so it
            // uniformly covers all of them with one rule, rather than
            // needing to be threaded through each one separately.
            if (report_errors && body.const_locals.contains(stmt.local) && state.locals.contains(stmt.local)) {
                throw DataflowError("cannot reassign 'const' variable '" + stmt.local + "' after initialization",
                    state.current_loc);
            }
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
                //
                // spec §6.4(3): a genuine reassignment is nonetheless
                // allowed when the RHS is any rvalue of the exact same
                // class type (`y = std::move(x);`, a by-value call result,
                // a factory expression, ...) -- dispatching to the
                // compiler-synthesized move assignment operator --
                // provided the class has no reference-typed member (spec
                // §6.4(3)'s own exception: a reference member can't be
                // re-seated by assignment, only ever bound once at
                // construction, mirroring real C++'s
                // [class.copy.assign]). Anything else (including a
                // same-class rvalue source for a class *with* a reference
                // member) falls through to the unconditional "no copy
                // semantics" rejection just below, unchanged.
                bool is_move_assignment = produces_rvalue_of_type(*stmt.expr, type_it->second, body, signatures);
                if (is_move_assignment && state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        bool has_reference_member = false;
                        if (state.class_field_types != nullptr) {
                            auto fields_it = state.class_field_types->find(type_it->second.name);
                            if (fields_it != state.class_field_types->end()) {
                                for (const auto& [field_name, field_type] : fields_it->second) {
                                    if (is_reference(field_type)) {
                                        has_reference_member = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (has_reference_member) {
                            throw DataflowError(
                                "class '" + type_it->second.name +
                                    "' has a reference-typed member, so it has no move assignment operator "
                                    "(spec §6.4(3)) -- '" + stmt.local + "' cannot be reassigned",
                                state.current_loc);
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            throw DataflowError("cannot assign to class variable '" + stmt.local +
                                                 "': it is currently borrowed",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures, report_errors);
                    state.locals[stmt.local] = LocalState::Initialized;
                    return;
                }
                // spec §6.5(3): `y = x;` (a bare, non-move reassignment)
                // is copy assignment when `x` is a plain variable of the
                // exact same class type -- licensed only when the class
                // is copy-assignable (user-declared or compiler-
                // eligible). A class ineligible for copy assignment
                // (e.g. one with a reference member, or one whose
                // destructor/copy-constructor declaration suppresses
                // auto-generation with no user-declared operator=) falls
                // through to the unconditional "no copy semantics"
                // rejection just below, unchanged. Unlike move
                // assignment, copying never changes `x`'s own state
                // (spec §6.5's own note) -- no MovedOut transition for
                // it, so apply_expr is called with is_move_target_context
                // irrelevant here (there is no std::move to license).
                if (is_bare_same_type_copy_source(*stmt.expr, type_it->second, body) &&
                    state.locals.contains(stmt.local)) {
                    if (report_errors) {
                        if (state.classes_with_copy_assign == nullptr ||
                            !state.classes_with_copy_assign->contains(type_it->second.name)) {
                            throw DataflowError("class '" + type_it->second.name +
                                                 "' is not copy-assignable (spec §6.5(3)) -- '" + stmt.local +
                                                 "' cannot be reassigned this way",
                                state.current_loc);
                        }
                        auto borrow_it = state.borrows.find(stmt.local);
                        if (borrow_it != state.borrows.end() &&
                            (borrow_it->second.mutable_borrow || borrow_it->second.shared_count > 0)) {
                            throw DataflowError("cannot assign to class variable '" + stmt.local +
                                                 "': it is currently borrowed",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
                    state.locals[stmt.local] = LocalState::Initialized;
                    return;
                }
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
                    std::vector<ClosureCaptureBorrow> closure_capture_borrows;
                    apply_lambda_captures(*stmt.expr, state, state.borrows, body, signatures, report_errors,
                                          &closure_capture_borrows);
                    if (!closure_capture_borrows.empty()) {
                        state.closure_capture_borrows[stmt.local] = std::move(closure_capture_borrows);
                    }
                } else {
                    if (produces_rvalue_of_type(*stmt.expr, type_it->second, body, signatures)) {
                        apply_expr(*stmt.expr, /*is_move_target_context=*/true, state, body, signatures,
                                   report_errors);
                        state.locals[stmt.local] = LocalState::Initialized;
                        return;
                    }
                    // spec §6.5: `ClassName y = x;` (a bare, non-move,
                    // non-lambda initializer -- this variable's first-
                    // ever appearance, per the surrounding comment) is
                    // copy construction when `x` is a plain variable of
                    // the exact same class type -- licensed only when
                    // the class is copy-constructible (user-declared or
                    // compiler-eligible, spec §6.5(2)/is_copy_
                    // constructible). Recognizes exactly the shape spec
                    // §6.5's own worked example uses (`Foo b = a;`);
                    // anything else (a different type, a non-plain-
                    // variable expression) is rejected -- this used to be
                    // an entirely unchecked, silent bitwise copy for
                    // *any* expression shape at all (a real gap, closed
                    // as part of implementing this feature).
                    if (report_errors) {
                        if (!is_bare_same_type_copy_source(*stmt.expr, type_it->second, body)) {
                            throw DataflowError(
                                "class '" + type_it->second.name + "'-typed variable '" + stmt.local +
                                    "' can only be initialized via constructor-call syntax ('" +
                                    type_it->second.name + " " + stmt.local +
                                    "(args);'), std::move of the same type, or (if the class is copy-"
                                    "constructible, spec §6.5) a plain copy of another '" + type_it->second.name +
                                    "' variable",
                                state.current_loc);
                        }
                        if (state.classes_with_copy_ctor == nullptr ||
                            !state.classes_with_copy_ctor->contains(type_it->second.name)) {
                            throw DataflowError("class '" + type_it->second.name +
                                                 "' is not copy-constructible (spec §6.5(2)) -- '" + stmt.local +
                                                 "' cannot be initialized this way",
                                state.current_loc);
                        }
                    }
                    apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures,
                               report_errors);
                }
                state.locals[stmt.local] = LocalState::Initialized;
                return;
            }

            apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
            if (type_it != body.local_types.end()) {
                check_function_pointer_assignment(type_it->second, *stmt.expr, body, signatures, state.current_loc,
                                                  stmt.local, report_errors);
                check_raw_pointer_assignment(type_it->second, *stmt.expr, body, signatures, state.current_loc,
                                             stmt.local, report_errors);
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
            apply_expr(*stmt.expr, /*is_move_target_context=*/false, state, body, signatures, report_errors);
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
            // ScopeExit (if any) is ever reached, so "drop a still-
            // borrowed local at scope exit" can't arise here either.
            release_reference_borrow(stmt.local, state, body);
            release_closure_capture_borrows(stmt.local, state);
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
            bool return_is_class_value = is_named_class_type(fn.return_type, body);
            bool copyable_lvalue_source =
                return_is_class_value && is_copyable_class_lvalue_boundary_source(*term.return_value, fn.return_type, body);
            apply_expr(*term.return_value, return_is_class_value && !copyable_lvalue_source, state, body, signatures,
                       /*report_errors=*/true);
            if (return_is_class_value && !copyable_lvalue_source &&
                !produces_rvalue_of_type(*term.return_value, fn.return_type, body, signatures)) {
                throw DataflowError("returning class '" + fn.return_type.name +
                                     "' by value requires either a copyable bare local of that exact type or "
                                     "a fresh value such as std::move(x) or a call returning by value",
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
void check_function(const Function& fn, const Program& program, const Signatures& signatures,
                     const std::unordered_set<std::string>& class_names,
                     const ClassFieldTypes& class_field_types, const ClassFieldAccess& class_field_access,
                     const std::unordered_set<std::string>& classes_with_copy_ctor,
                     const std::unordered_set<std::string>& classes_with_copy_assign,
                     const std::unordered_set<std::string>& witness_class_names) {
    Body body = build_mir(fn);
    body.program = &program;

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
    // ch01 §1.2/§1.3: every function is checked by default,
    // unconditionally -- there is no per-function way to start already
    // inside an implicit unsafe context via mere absence of any marker
    // (the old "native function" concept is fully retired). A function
    // whose own declaration carries the function-level `[[scpp::unsafe]]`
    // marker (`fn.is_unsafe`) is the one explicit exception: its entire
    // body is an unsafe context throughout, exactly as if that whole
    // body were itself wrapped in one `[[scpp::unsafe]] { }` block --
    // implemented identically, by starting unsafe_depth at 1 instead of
    // 0 (an ordinary nested block further increments/decrements this
    // same counter from whatever it started at, so nesting one inside
    // an already-unsafe function's body is harmless, matching ch01
    // §1.2's "neither form changes §5.1-§5.4's checking" guarantee).
    // Every other function's entry_state starts at 0; unsafe_depth then
    // only increases via an explicit, lexically nested
    // `[[scpp::unsafe]] { }` block within that function's own body.
    entry_state.unsafe_depth = fn.is_unsafe ? 1 : 0;
    // ch04 §4.2/ch05 §5.9: `this` is always params[0] when present (see
    // parser's make_this_param) -- a user can never spell a same-named
    // parameter themselves, since `this` is a keyword, not an ordinary
    // identifier token.
    if (!fn.params.empty() && fn.params[0].name == "this") {
        entry_state.current_class = fn.params[0].type.pointee->name;
    }
    entry_state.class_names = &class_names;
    entry_state.class_field_types = &class_field_types;
    entry_state.class_field_access = &class_field_access;
    entry_state.classes_with_copy_ctor = &classes_with_copy_ctor;
    entry_state.classes_with_copy_assign = &classes_with_copy_assign;
    for (const Param& param : fn.params) {
        entry_state.locals[param.name] = LocalState::Initialized;
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
        sig.is_extern_c_declaration_only = fn.is_extern_c && fn.body == nullptr;
        sig.is_unsafe = fn.is_unsafe;
        sig.loc = fn.loc;
        sig.receiver_ref_qualifier = fn.receiver_ref_qualifier;
        std::vector<FunctionSignature>& overloads = signatures[fn.name];
        for (const FunctionSignature& existing : overloads) {
            bool same_params = existing.param_types.size() == sig.param_types.size();
            for (size_t i = 0; same_params && i < sig.param_types.size(); i++) {
                same_params = types_equal(existing.param_types[i], sig.param_types[i]);
            }
            if (same_params && existing.receiver_ref_qualifier == sig.receiver_ref_qualifier) {
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
    clone->explicit_template_args.reserve(expr.explicit_template_args.size());
    for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
        ExplicitTemplateArg cloned_arg;
        cloned_arg.is_type = arg.is_type;
        cloned_arg.type = arg.type;
        if (arg.value) cloned_arg.value = std::shared_ptr<Expr>(clone_expr(*arg.value).release());
        clone->explicit_template_args.push_back(std::move(cloned_arg));
    }
    clone->type = expr.type;
    clone->has_paren_init = expr.has_paren_init;
    clone->fold_ellipsis_on_left = expr.fold_ellipsis_on_left;
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

// ch05 §5.14: `Function` has no copy constructor at all (its `body` is
// a move-only `StmtPtr`) -- this is the closest equivalent, deep-cloning
// the body (via clone_stmt) while plainly copying every other, already-
// copyable field. Used by method_templates_of to hand back independent
// copies (never references into program_.functions' own backing
// storage, which the generic-type monomorphization machinery may
// reallocate out from under a held reference -- see this file's other
// generic-type methods' identical concern).
[[nodiscard]] Function clone_function(const Function& fn) {
    Function clone;
    clone.return_type = fn.return_type;
    clone.name = fn.name;
    clone.loc = fn.loc;
    clone.params = fn.params;
    clone.body = fn.body ? clone_stmt(*fn.body) : nullptr;
    clone.is_extern_c = fn.is_extern_c;
    clone.is_module_extern = fn.is_module_extern;
    clone.is_unsafe = fn.is_unsafe;
    clone.has_varargs = fn.has_varargs;
    clone.method_requires_concept = fn.method_requires_concept;
    clone.is_generic_template = fn.is_generic_template;
    clone.template_params = fn.template_params;
    clone.generic_method_owner_id = fn.generic_method_owner_id;
    clone.receiver_ref_qualifier = fn.receiver_ref_qualifier;
    clone.namespace_path = fn.namespace_path;
    clone.is_exported = fn.is_exported;
    clone.owning_module = fn.owning_module;
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
        case TypeKind::Named: {
            if (type.template_args.empty()) return type.name;
            std::string result = type.name;
            for (const Type& arg : type.template_args) result += "_" + mangle_type_for_clone_name(arg);
            return result;
        }
        case TypeKind::Pointer:
            return mangle_type_for_clone_name(*type.pointee) + (type.is_mutable_pointee ? "_ptr" : "_cptr");
        case TypeKind::Function: {
            std::string result = mangle_type_for_clone_name(*type.function_return) + "_fntype";
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            if (type.is_const_function) result += "_const";
            if (type.function_ref_qualifier == ReceiverRefQualifier::LValue) result += "_lrefq";
            if (type.function_ref_qualifier == ReceiverRefQualifier::RValue) result += "_rrefq";
            return result;
        }
        case TypeKind::FunctionPointer: {
            std::string result = mangle_type_for_clone_name(*type.function_return) +
                                 (type.is_unsafe_function_pointer ? "_ufnptr" : "_fnptr");
            for (const Type& param : type.function_params) result += "_" + mangle_type_for_clone_name(param);
            return result;
        }
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
    if (expr.third) collect_free_identifiers(*expr.third, excluded, out);
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
        case StmtKind::Break:
        case StmtKind::Continue:
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
    if (expr.third) rewrite_captured_identifiers_as_field_access(*expr.third, captured_names);
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
        case StmtKind::Break:
        case StmtKind::Continue:
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
    if (expr.third) reject_write_to_nonmutable_by_value_capture(*expr.third, by_value_names);
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
        case StmtKind::Break:
        case StmtKind::Continue:
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
        for (size_t i = 0; i < program.classes.size(); i++) {
            const ClassDef& c = program.classes[i];
            known_type_names_.insert(c.name);
            if (!c.template_owner_id.empty()) {
                class_template_indices_by_owner_id_[c.template_owner_id] = i;
                if (!c.is_variadic_primary_template && !c.is_variadic_specialization) {
                    ordinary_class_template_owner_ids_by_name_[c.name].push_back(c.template_owner_id);
                }
            }
        }
        for (const ConceptDef& c : program.concepts) known_type_names_.insert(c.name);
        for (const Function& fn : program.functions) known_function_names_.insert(fn.name);
        // ch05 §5.14: every generic class/struct *template*'s own name --
        // used to (a) skip its own unresolved-"T" methods from every
        // other pass in this file (movecheck's Body-based machinery has
        // no way to make sense of a type that isn't real anywhere in
        // the program) and (b) recognize a Type::template_args-bearing
        // Type as naming one of *these* specifically (parser.cppm's own
        // generic_type_names_ already guarantees nothing else could).
        for (const ClassDef& c : program.classes) {
            if (!c.template_params.empty()) generic_type_template_names_.insert(c.name);
        }
        for (const StructDef& s : program.structs) {
            if (!s.template_params.empty()) generic_type_template_names_.insert(s.name);
        }
        // ch05 §5.14: every variadic generic type's own *primary
        // template* name (e.g. "Tuple") -- distinguishes a variadic
        // instantiation (`Tuple<int,bool,char>`, resolved by
        // instantiate_variadic_generic_type, one concrete ClassDef per
        // recursive-inheritance level) from an ordinary, single-type-
        // parameter one (`Vec<int>`, resolved by instantiate_generic_
        // type). A variadic primary template's own name is always
        // already a member of generic_type_template_names_ too (its
        // own template_params is non-empty, a single pack parameter),
        // but the two mechanisms are otherwise unrelated -- this set is
        // what resolve_generic_type actually branches on.
        for (const ClassDef& c : program.classes) {
            if (c.is_variadic_primary_template) variadic_generic_type_names_.insert(c.name);
        }
    }

    void run() {
        signatures_ = build_signatures(program_);
        // ch05 §5.14: synthesizes a "forwarding stub" Function for every
        // inherited method/field access a derived class doesn't itself
        // override (see synthesize_inherited_method_forwards' own
        // comment) -- runs first, since resolve_generic_types/
        // check_generic_type_methods_once and the ordinary per-function
        // walk below all resolve an inherited method call by simply
        // finding "DerivedClass_methodName" already present in
        // program_.functions, exactly like an ordinary, non-inherited
        // method -- no inheritance-specific fallback logic needed
        // anywhere else in this file (or in codegen) as a result.
        synthesize_inherited_method_forwards();
        // ch05 §5.14: resolves every `GenericType<Concrete>` instantiation
        // anywhere in the program (struct/class fields, every function/
        // method's own signature, and every VarDecl inside a body) and
        // checks every generic class's own methods once, abstractly, at
        // their own definition -- both *before* the rest of this pass,
        // since neither depends on anything it does, and the ordinary
        // per-function walk just below would otherwise trip over an
        // unresolved generic-type Named type it can't make sense of.
        resolve_generic_types();
        check_generic_type_methods_once();
        // resolve_generic_types/check_generic_type_methods_once may
        // monomorphize generic types and rewrite existing function
        // signatures/return types (e.g. `MyBox<int>` -> its concrete
        // synthesized class name). Rebuild the signature table before any
        // later overload resolution or generic-function constraint check
        // consults it, so those queries see the fully concrete program
        // shape rather than the pre-resolution templates.
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
        //
        // ch05 §5.14: a generic *type*'s own template methods (their
        // `this` parameter names an unresolved generic-type template
        // directly, never a witness) are skipped entirely here -- "T"
        // is never a real type anywhere in the program for them, so
        // there is nothing this walk (or check_moves/codegen after it)
        // could safely do with one; they exist purely as a body/
        // signature source for check_generic_type_methods_once and
        // resolve_generic_types' own per-instantiation clones, both
        // already done above.
        size_t original_count = program_.functions.size();
        for (size_t i = 0; i < original_count; i++) {
            if (program_.functions[i].body == nullptr) continue;
            if (belongs_to_unresolved_generic_type_template(program_.functions[i])) continue;
            // ch05 §5.11: a full-header-form generic function's own
            // template (Function::template_params non-empty, e.g.
            // `get`/`make`) is skipped here too, for the identical
            // reason -- its own body may reference a not-yet-bound
            // template parameter's own name as a type (or, for a
            // base-class-deduction pattern, something with no concrete
            // meaning at all outside a real call site); see
            // check_moves's own identical guard/comment. Each concrete
            // call site is instead resolved directly below (this same
            // walk, over an *already-monomorphized* caller's own body)
            // by monomorphize_generic_function_call.
            if (!program_.functions[i].template_params.empty()) continue;
            // build_mir's own Body holds raw (const Expr*) pointers into
            // this Function's *own* Stmt/Expr tree (see mir.cppm's
            // Terminator) -- safe to keep using after program_.functions
            // mutates below, since only the *vector's* backing storage
            // (and, incidentally, the Function objects it directly
            // holds) can move; a Function's own body is heap-allocated
            // independently (via StmtPtr/ExprPtr) and never relocates
            // just because the enclosing vector reallocates elsewhere.
            Body body = build_mir(program_.functions[i]);
            body.program = &program_;
            bool allow_generic_monomorphization = !program_.functions[i].is_generic_template;
            walk_stmt(*program_.functions[i].body, body, this_type_of(program_.functions[i]),
                      allow_generic_monomorphization);
        }
    }

private:
    Program& program_;
    std::unordered_map<std::string, const ConceptDef*> concepts_by_name_;
    std::unordered_map<std::string, size_t> generic_template_indices_;
    std::unordered_map<std::string, size_t> class_template_indices_by_owner_id_;
    std::unordered_map<std::string, std::vector<std::string>> ordinary_class_template_owner_ids_by_name_;
    std::unordered_map<std::string, std::string> clone_cache_;
    std::unordered_set<std::string> known_type_names_;
    std::unordered_set<std::string> known_function_names_;
    Signatures signatures_;
    // ch05 §5.14: every generic class/struct template's own name -- see
    // the constructor's own comment.
    std::unordered_set<std::string> generic_type_template_names_;
    // ch05 §5.14: every variadic generic type's own primary-template
    // name -- see the constructor's own comment.
    std::unordered_set<std::string> variadic_generic_type_names_;
    // ch05 §5.14: caches an already-synthesized concrete generic-type
    // instantiation by "TemplateName.MangledArgType" (mirrors
    // clone_cache_'s identical purpose for generic functions), so
    // `Vec<int>` used twice in the same program shares one concrete
    // class/method set rather than duplicating it.
    std::unordered_map<std::string, std::string> generic_type_instance_cache_;
    // ch05 §5.14: every concrete variadic-generic-type instantiation's
    // own recorded (non-type argument values, type arguments) --
    // populated by instantiate_variadic_generic_type, keyed by the
    // concrete ClassDef's own (mangled) name. The *only* way base-class
    // deduction (monomorphize_generic_function_call's own
    // deduce_via_base_class_chain) can recover "what was this level's
    // own Head/Tail/Idx" after the fact: a concrete ClassDef itself
    // records only its own *fields* (already-substituted types), not
    // which template arguments produced them.
    struct VariadicInstanceInfo {
        std::string template_name;
        std::vector<int> non_type_values;
        std::vector<Type> type_args;
    };
    std::unordered_map<std::string, VariadicInstanceInfo> variadic_instance_info_;
    // ch05 §5.11: caches an already-synthesized concrete monomorphized
    // clone of a full-header-form generic *function* template (e.g.
    // `get`/`make`) by its own template-parameter-binding cache key --
    // see monomorphize_generic_function_call's own comment. Kept
    // separate from clone_cache_ (the abbreviated-Concept-auto-form's
    // own identical-purpose cache) since the two forms' cache keys are
    // computed differently (this one keys off the *template parameter
    // bindings themselves*, not the concrete function-parameter types --
    // the two diverge for a base-class-deduction parameter, whose
    // concrete parameter type is the *deduced base*, not any input the
    // cache key would otherwise naturally be built from).
    std::unordered_map<std::string, std::string> generic_function_clone_cache_;
    // ch05 §5.14: the single, shared, globally-empty witness struct
    // representing a completely bare (unconstrained) generic-type
    // parameter -- see check_generic_type_methods_once's own comment.
    // Empty until first needed (lazily synthesized), since most
    // programs have no bare generic type at all.
    std::string bare_witness_struct_name_;
    // ch05 §5.14: a monotonically-increasing counter for synthesizing
    // each generic method's own unique "checking class" name
    // ("__genchk0", "__genchk1", ...) in check_generic_type_methods_once
    // -- mirrors lambda_counter_'s identical purpose/reasoning.
    int generic_check_counter_ = 0;
    // ch05 §5.12: a monotonically-increasing counter for synthesizing
    // each closure's own unique class name ("__lambda0", "__lambda1",
    // ...) -- a lambda literal has no user-spelled name to reuse (unlike
    // a concept's witness class, which shares the concept's own name),
    // and this codebase has no other source of process-wide uniqueness
    // to draw on.
    int lambda_counter_ = 0;

    // ch05 §5.14: true when `fn` is one of a generic class/struct
    // template's own, not-yet-resolved methods (its `this` parameter
    // names the template directly, e.g. "Vec", never a witness or a
    // concrete instantiation like "Vec_int") -- "T" is never a real
    // type anywhere in the program for these, so every other pass in
    // this file must skip them entirely (see run()'s own comment).
    [[nodiscard]] bool belongs_to_unresolved_generic_type_template(const Function& fn) const {
        return !fn.generic_method_owner_id.empty();
    }

    struct TemplateInstantiationBindings {
        std::vector<std::pair<std::string, Type>> type_replacements;
        std::unordered_map<std::string, std::vector<Type>> type_pack_replacements;
    };

    [[nodiscard]] const ClassDef* class_template_by_owner_id(const std::string& owner_id) const {
        auto it = class_template_indices_by_owner_id_.find(owner_id);
        if (it == class_template_indices_by_owner_id_.end()) return nullptr;
        return &program_.classes[it->second];
    }

    [[nodiscard]] static std::string method_suffix_after_owner_prefix(const Function& fn, const std::string& class_name,
                                                                       const std::string& owner_id) {
        std::string owner_prefix = owner_id.empty() ? class_name : class_name + "__" + owner_id;
        if (fn.name.rfind(owner_prefix, 0) == 0) return fn.name.substr(owner_prefix.size());
        if (fn.name.rfind(class_name, 0) == 0) return fn.name.substr(class_name.size());
        return fn.name;
    }

    void walk_new_concrete_function(size_t fn_index) {
        if (fn_index >= program_.functions.size()) return;
        Function& fn = program_.functions[fn_index];
        if (fn.body == nullptr || !fn.template_params.empty()) return;
        signatures_ = build_signatures(program_);
        Body body = build_mir(fn);
        body.program = &program_;
        walk_stmt(*fn.body, body, this_type_of(fn), /*allow_generic_monomorphization=*/!fn.is_generic_template);
    }

    // ch04 §4.2/ch05 §5.9: the enclosing function's own `this` parameter
    // type (Named(ClassName)), or nullopt if `fn` isn't a method at all
    // -- used to type a `[this]` lambda capture. `this` is always
    // params[0] when present (parser's make_this_param).
    [[nodiscard]] static std::optional<Type> this_type_of(const Function& fn) {
        if (fn.params.empty() || fn.params[0].name != "this") return std::nullopt;
        return *fn.params[0].type.pointee;
    }

    // ch05 §5.14: replaces every occurrence of the generic type
    // parameter named `param_name` (a plain Named type, e.g. "T")
    // inside `type` with `replacement` -- used both to substitute a
    // real concrete argument (resolve_generic_types/instantiate_generic_
    // type) and a witness class (check_generic_type_methods_once), the
    // same way a generic function's own Concept-constrained parameter
    // is substituted at its own call site.
    [[nodiscard]] static Type substitute_type_param(const Type& type, const std::string& param_name,
                                                     const Type& replacement) {
        if (type.kind == TypeKind::Named && type.name == param_name) return replacement;
        Type result = type;
        for (Type& arg : result.template_args) {
            arg = substitute_type_param(arg, param_name, replacement);
        }
        if (result.pointee) {
            result.pointee = std::make_shared<Type>(substitute_type_param(*result.pointee, param_name, replacement));
        }
        if (result.element) {
            result.element = std::make_shared<Type>(substitute_type_param(*result.element, param_name, replacement));
        }
        if (result.function_return) {
            result.function_return =
                std::make_shared<Type>(substitute_type_param(*result.function_return, param_name, replacement));
        }
        for (Type& param : result.function_params) {
            param = substitute_type_param(param, param_name, replacement);
        }
        return result;
    }

    [[nodiscard]] static Type substitute_type_params(
        const Type& type, const std::vector<std::pair<std::string, Type>>& replacements) {
        Type result = type;
        for (const auto& [param_name, replacement] : replacements) {
            result = substitute_type_param(result, param_name, replacement);
        }
        return result;
    }

    [[nodiscard]] static std::optional<std::string>
    referenced_type_pack_param_name(const Type& type, const std::vector<GenericTypeParam>& template_params) {
        if (type.kind == TypeKind::Named) {
            for (const GenericTypeParam& tp : template_params) {
                if (tp.is_pack && !tp.is_non_type && tp.name == type.name) return tp.name;
            }
        }
        if (type.pointee) {
            if (std::optional<std::string> found =
                    referenced_type_pack_param_name(*type.pointee, template_params)) {
                return found;
            }
        }
        if (type.element) {
            if (std::optional<std::string> found =
                    referenced_type_pack_param_name(*type.element, template_params)) {
                return found;
            }
        }
        return std::nullopt;
    }

    // ch05 §5.14: applies substitute_type_param to every Type appearing
    // anywhere inside `expr`'s own sub-tree (currently only MakeUnique's
    // element type and a Lambda's own explicit return type carry a
    // meaningful `.type` -- substituting it on every other expr kind is
    // a harmless no-op, since their own `.type` is left default-
    // constructed and could never match `param_name` anyway) -- needed
    // because clone_stmt/clone_expr copy a generic method's own body
    // verbatim, "T" and all, so every Type embedded *inside* the body
    // (not just the method's own signature, handled separately by its
    // caller) must be substituted too.
    void substitute_type_param_in_expr(Expr& expr, const std::string& param_name, const Type& replacement) {
        expr.type = substitute_type_param(expr.type, param_name, replacement);
        for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
            if (arg.is_type) {
                arg.type = substitute_type_param(arg.type, param_name, replacement);
            } else if (arg.value) {
                substitute_type_param_in_expr(*arg.value, param_name, replacement);
            }
        }
        if (expr.lhs) substitute_type_param_in_expr(*expr.lhs, param_name, replacement);
        if (expr.rhs) substitute_type_param_in_expr(*expr.rhs, param_name, replacement);
        for (ExprPtr& arg : expr.args) substitute_type_param_in_expr(*arg, param_name, replacement);
        for (Param& p : expr.lambda_params) p.type = substitute_type_param(p.type, param_name, replacement);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) substitute_type_param_in_expr(*c.init, param_name, replacement);
        }
        if (expr.lambda_body) substitute_type_param_in_stmt(*expr.lambda_body, param_name, replacement);
    }

    void substitute_type_params_in_expr(Expr& expr, const std::vector<std::pair<std::string, Type>>& replacements) {
        for (const auto& [param_name, replacement] : replacements) {
            substitute_type_param_in_expr(expr, param_name, replacement);
        }
    }

    void substitute_type_param_in_stmt(Stmt& stmt, const std::string& param_name, const Type& replacement) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                stmt.type = substitute_type_param(stmt.type, param_name, replacement);
                if (stmt.init) substitute_type_param_in_expr(*stmt.init, param_name, replacement);
                for (ExprPtr& arg : stmt.ctor_args) substitute_type_param_in_expr(*arg, param_name, replacement);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_type_param_in_expr(*stmt.expr, param_name, replacement);
                return;
            case StmtKind::If:
                substitute_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                if (stmt.else_branch) substitute_type_param_in_stmt(*stmt.else_branch, param_name, replacement);
                return;
            case StmtKind::While:
                substitute_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_type_param_in_stmt(*s, param_name, replacement);
                return;
        }
    }

    void substitute_type_params_in_stmt(Stmt& stmt, const std::vector<std::pair<std::string, Type>>& replacements) {
        for (const auto& [param_name, replacement] : replacements) {
            substitute_type_param_in_stmt(stmt, param_name, replacement);
        }
    }

    void substitute_non_type_param_in_expr(Expr& expr, const std::string& param_name, int replacement) {
        if (expr.kind == ExprKind::Identifier && expr.name == param_name) {
            expr.kind = ExprKind::IntegerLiteral;
            expr.int_value = replacement;
            expr.name.clear();
            expr.lhs.reset();
            expr.rhs.reset();
            expr.args.clear();
            expr.explicit_template_args.clear();
            return;
        }
        for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
            if (!arg.is_type && arg.value) substitute_non_type_param_in_expr(*arg.value, param_name, replacement);
        }
        if (expr.lhs) substitute_non_type_param_in_expr(*expr.lhs, param_name, replacement);
        if (expr.rhs) substitute_non_type_param_in_expr(*expr.rhs, param_name, replacement);
        for (ExprPtr& arg : expr.args) substitute_non_type_param_in_expr(*arg, param_name, replacement);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) substitute_non_type_param_in_expr(*c.init, param_name, replacement);
        }
        if (expr.lambda_body) substitute_non_type_param_in_stmt(*expr.lambda_body, param_name, replacement);
    }

    void substitute_non_type_param_in_stmt(Stmt& stmt, const std::string& param_name, int replacement) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) substitute_non_type_param_in_expr(*stmt.init, param_name, replacement);
                for (ExprPtr& arg : stmt.ctor_args) substitute_non_type_param_in_expr(*arg, param_name, replacement);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_non_type_param_in_expr(*stmt.expr, param_name, replacement);
                return;
            case StmtKind::If:
                substitute_non_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_non_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                if (stmt.else_branch) substitute_non_type_param_in_stmt(*stmt.else_branch, param_name, replacement);
                return;
            case StmtKind::While:
                substitute_non_type_param_in_expr(*stmt.condition, param_name, replacement);
                substitute_non_type_param_in_stmt(*stmt.then_branch, param_name, replacement);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_non_type_param_in_stmt(*s, param_name, replacement);
                return;
        }
    }

    [[nodiscard]] std::vector<Function> methods_of_type_name(const std::string& type_name) const {
        std::vector<Function> result;
        for (const Function& fn : program_.functions) {
            if (!fn.params.empty() && fn.params[0].name == "this" && fn.params[0].type.pointee != nullptr &&
                fn.params[0].type.pointee->name == type_name) {
                result.push_back(clone_function(fn));
            }
        }
        return result;
    }

    // ch05 §5.14: every method (including a constructor/destructor) still
    // attached to exactly one unresolved generic class template definition
    // or ordinary partial specialization pattern, identified by that
    // template's own internal owner id rather than its exposed class name.
    // This keeps distinct `function<...>` template definitions from
    // colliding once more than one shares the same `this` pointee spelling.
    [[nodiscard]] std::vector<Function> method_templates_of_owner(const std::string& owner_id) const {
        std::vector<Function> result;
        for (const Function& fn : program_.functions) {
            if (fn.generic_method_owner_id == owner_id) result.push_back(clone_function(fn));
        }
        return result;
    }

    [[nodiscard]] static const Type* find_type_replacement(const std::vector<std::pair<std::string, Type>>& replacements,
                                                           const std::string& name) {
        for (const auto& [param_name, replacement] : replacements) {
            if (param_name == name) return &replacement;
        }
        return nullptr;
    }

    [[nodiscard]] static Type instantiate_type_pattern(
        const Type& type, const std::vector<std::pair<std::string, Type>>& replacements,
        const std::unordered_map<std::string, std::vector<Type>>& pack_replacements) {
        if (!type.is_pack_expansion && type.kind == TypeKind::Named && type.template_args.empty() &&
            type.non_type_args.empty()) {
            if (const Type* replacement = find_type_replacement(replacements, type.name)) return *replacement;
        }
        Type result = type;
        result.is_pack_expansion = false;
        std::vector<Type> new_template_args;
        new_template_args.reserve(result.template_args.size());
        for (const Type& arg : result.template_args) {
            if (arg.is_pack_expansion && arg.kind == TypeKind::Named && pack_replacements.contains(arg.name)) {
                for (const Type& concrete : pack_replacements.at(arg.name)) new_template_args.push_back(concrete);
                continue;
            }
            new_template_args.push_back(instantiate_type_pattern(arg, replacements, pack_replacements));
        }
        result.template_args = std::move(new_template_args);
        if (result.pointee) {
            result.pointee =
                std::make_shared<Type>(instantiate_type_pattern(*result.pointee, replacements, pack_replacements));
        }
        if (result.element) {
            result.element =
                std::make_shared<Type>(instantiate_type_pattern(*result.element, replacements, pack_replacements));
        }
        if (result.function_return) {
            result.function_return = std::make_shared<Type>(
                instantiate_type_pattern(*result.function_return, replacements, pack_replacements));
        }
        std::vector<Type> new_function_params;
        new_function_params.reserve(result.function_params.size());
        for (const Type& param : result.function_params) {
            if (param.is_pack_expansion && param.kind == TypeKind::Named && pack_replacements.contains(param.name)) {
                for (const Type& concrete : pack_replacements.at(param.name)) new_function_params.push_back(concrete);
                continue;
            }
            new_function_params.push_back(instantiate_type_pattern(param, replacements, pack_replacements));
        }
        result.function_params = std::move(new_function_params);
        return result;
    }

    [[nodiscard]] static bool bind_type_pattern(
        const std::string& name, const Type& concrete, TemplateInstantiationBindings& bindings) {
        if (const Type* existing = find_type_replacement(bindings.type_replacements, name)) {
            return types_equal(*existing, concrete);
        }
        bindings.type_replacements.emplace_back(name, concrete);
        return true;
    }

    [[nodiscard]] static bool bind_type_pack_pattern(
        const std::string& name, const std::vector<Type>& concretes, TemplateInstantiationBindings& bindings) {
        auto it = bindings.type_pack_replacements.find(name);
        if (it != bindings.type_pack_replacements.end()) {
            if (it->second.size() != concretes.size()) return false;
            for (size_t i = 0; i < concretes.size(); i++) {
                if (!types_equal(it->second[i], concretes[i])) return false;
            }
            return true;
        }
        bindings.type_pack_replacements[name] = concretes;
        return true;
    }

    [[nodiscard]] static bool match_type_pattern_list(
        const std::vector<Type>& patterns, const std::vector<Type>& concretes, const std::vector<GenericTypeParam>& params,
        TemplateInstantiationBindings& bindings) {
        std::function<bool(const Type&, const Type&)> match_one;
        std::function<bool(const std::vector<Type>&, const std::vector<Type>&)> match_list;
        match_list = [&](const std::vector<Type>& inner_patterns, const std::vector<Type>& inner_concretes) -> bool {
            if (!inner_patterns.empty()) {
                const Type& last = inner_patterns.back();
                if (last.is_pack_expansion && last.kind == TypeKind::Named) {
                    for (const GenericTypeParam& param : params) {
                        if (!param.is_pack || param.is_non_type || param.name != last.name) continue;
                        if (inner_concretes.size() + 1 < inner_patterns.size()) return false;
                        for (size_t i = 0; i + 1 < inner_patterns.size(); i++) {
                            if (!match_one(inner_patterns[i], inner_concretes[i])) return false;
                        }
                        std::vector<Type> pack_slice(
                            inner_concretes.begin() + static_cast<std::ptrdiff_t>(inner_patterns.size() - 1),
                            inner_concretes.end());
                        return bind_type_pack_pattern(param.name, pack_slice, bindings);
                    }
                }
            }
            if (inner_patterns.size() != inner_concretes.size()) return false;
            for (size_t i = 0; i < inner_patterns.size(); i++) {
                if (!match_one(inner_patterns[i], inner_concretes[i])) return false;
            }
            return true;
        };
        match_one = [&](const Type& pattern, const Type& concrete) -> bool {
            if (!pattern.is_pack_expansion && pattern.kind == TypeKind::Named && pattern.template_args.empty() &&
                pattern.non_type_args.empty()) {
                for (const GenericTypeParam& param : params) {
                    if (param.is_non_type || param.is_pack || param.name != pattern.name) continue;
                    return bind_type_pattern(param.name, concrete, bindings);
                }
            }
            if (pattern.kind != concrete.kind) return false;
            switch (pattern.kind) {
                case TypeKind::Named:
                    if (pattern.name != concrete.name || pattern.non_type_args.size() != concrete.non_type_args.size()) {
                        return false;
                    }
                    return match_list(pattern.template_args, concrete.template_args);
                case TypeKind::Pointer:
                    return pattern.is_mutable_pointee == concrete.is_mutable_pointee &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Reference:
                    return pattern.is_mutable_ref == concrete.is_mutable_ref &&
                           pattern.is_rvalue_ref == concrete.is_rvalue_ref &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Span:
                    return pattern.is_mutable_ref == concrete.is_mutable_ref &&
                           match_one(*pattern.pointee, *concrete.pointee);
                case TypeKind::Array:
                    return pattern.array_size == concrete.array_size &&
                           match_one(*pattern.element, *concrete.element);
                case TypeKind::FunctionPointer:
                    if (pattern.is_unsafe_function_pointer != concrete.is_unsafe_function_pointer) return false;
                    [[fallthrough]];
                case TypeKind::Function:
                    return pattern.is_const_function == concrete.is_const_function &&
                           pattern.function_ref_qualifier == concrete.function_ref_qualifier &&
                           match_one(*pattern.function_return, *concrete.function_return) &&
                           match_list(pattern.function_params, concrete.function_params);
            }
            return false;
        };
        return match_list(patterns, concretes);
    }

    struct OrdinaryClassTemplateSelection {
        const ClassDef* def = nullptr;
        TemplateInstantiationBindings bindings;
    };

    [[nodiscard]] OrdinaryClassTemplateSelection select_ordinary_class_template(
        const std::string& template_name, const std::vector<Type>& concrete_args, SourceLocation loc) const {
        OrdinaryClassTemplateSelection primary_selection;
        bool have_primary_definition = false;
        bool have_primary_forward_decl = false;
        std::vector<OrdinaryClassTemplateSelection> matching_specializations;

        auto owner_it = ordinary_class_template_owner_ids_by_name_.find(template_name);
        if (owner_it == ordinary_class_template_owner_ids_by_name_.end()) {
            return primary_selection;
        }

        for (const std::string& owner_id : owner_it->second) {
            const ClassDef* candidate = class_template_by_owner_id(owner_id);
            if (candidate == nullptr || candidate->is_variadic_primary_template || candidate->is_variadic_specialization) {
                continue;
            }
            if (candidate->is_partial_specialization) {
                if (candidate->specialization_template_args.size() != concrete_args.size()) continue;
                TemplateInstantiationBindings bindings;
                if (!match_type_pattern_list(candidate->specialization_template_args, concrete_args, candidate->template_params,
                                             bindings)) {
                    continue;
                }
                matching_specializations.push_back(OrdinaryClassTemplateSelection{candidate, std::move(bindings)});
                continue;
            }
            if (candidate->template_params.size() != concrete_args.size()) continue;
            TemplateInstantiationBindings bindings;
            bool valid = true;
            for (size_t param_index = 0; param_index < candidate->template_params.size(); ++param_index) {
                const GenericTypeParam& param = candidate->template_params[param_index];
                if (param.is_non_type || param.is_pack) {
                    valid = false;
                    break;
                }
                bindings.type_replacements.emplace_back(param.name, concrete_args[param_index]);
            }
            if (!valid) continue;
            OrdinaryClassTemplateSelection selection{candidate, std::move(bindings)};
            if (candidate->is_forward_declaration) {
                if (!have_primary_definition) {
                    primary_selection = std::move(selection);
                    have_primary_forward_decl = true;
                }
            } else {
                primary_selection = std::move(selection);
                have_primary_definition = true;
            }
        }

        if (matching_specializations.size() > 1) {
            throw DataflowError("multiple partial specializations of '" + template_name +
                                    "' match this concrete argument list; this version requires an unambiguous "
                                    "single best match",
                                loc);
        }
        if (!matching_specializations.empty()) return matching_specializations.front();
        if (have_primary_definition) return primary_selection;
        if (have_primary_forward_decl) {
            throw DataflowError("'" + template_name +
                                    "' has no matching class-template definition for these concrete arguments "
                                    "(the primary template is only forward-declared)",
                                loc);
        }
        return primary_selection;
    }

    // ch05 §5.14: for every class with a base (ClassDef::base_class_name),
    // synthesizes a "forwarding stub" Function (Function::forwards_to)
    // for every method the base class defines (recursively -- including
    // any forward the base class itself already synthesized from *its*
    // own base, since classes are processed in the same declaration
    // order the parser already requires: a base is always fully
    // declared -- and thus already has its own forwards synthesized --
    // before a class inheriting from it) that the derived class doesn't
    // itself override. This means every other pass in this file (and
    // every codegen call-resolution path) resolves an inherited method
    // call by simply finding "DerivedClass_methodName" already present
    // in program_.functions, exactly like an ordinary, non-inherited
    // method -- no separate inheritance-aware fallback logic needed
    // anywhere else. A constructor/destructor ("_new"/"_delete") is
    // never forwarded: a derived class with no constructor of its own
    // already zero-initializes its whole (flattened, base-first)
    // layout by default, which already correctly zero-initializes the
    // inherited base fields too (see declare_class); synthesizing a
    // same-named forwarding constructor would just redundantly re-run
    // the base's own initialization a second time over the exact same
    // `this`, and would get in the way of a derived class later
    // defining its own constructor with a different parameter list.
    void synthesize_inherited_method_forwards() {
        size_t original_class_count = program_.classes.size();
        for (size_t i = 0; i < original_class_count; i++) {
            if (program_.classes[i].base_class_name.empty()) continue;
            // ch05 §5.14: a variadic specialization's own base_class_name
            // (e.g. "Tuple", set by parse_variadic_specialization's base-
            // clause handling) names the *template*, not a real,
            // concrete base class -- there is nothing to forward yet
            // (neither of the doc's own variadic examples defines a
            // method on a specialization at all; see this class's own
            // instantiate_variadic_generic_type comment). The real,
            // concrete per-level base chain is instead built directly
            // by instantiate_variadic_generic_type once resolve_generic_
            // types actually instantiates a concrete `Tuple<...>` --
            // skipped here explicitly rather than relying on
            // method_templates_of happening to return empty.
            if (program_.classes[i].is_variadic_primary_template || program_.classes[i].is_variadic_specialization) {
                continue;
            }
            std::string derived_name = program_.classes[i].name;
            std::string base_name = program_.classes[i].base_class_name;
            std::vector<std::string> namespace_path_copy = program_.classes[i].namespace_path;
            bool is_exported_copy = program_.classes[i].is_exported;

            std::vector<Function> base_methods = methods_of_type_name(base_name);
            for (const Function& base_method : base_methods) {
                // e.g. "_foo" out of "Circle_foo" -- see
                // method_templates_of's exact-name-match filter, which
                // guarantees this is always a clean prefix.
                std::string suffix = base_method.name.substr(base_name.size());
                if (suffix == "_new" || suffix == "_delete") continue;
                std::string derived_method_name = derived_name + suffix;
                bool already_defined = false;
                for (const Function& fn : program_.functions) {
                    if (fn.name == derived_method_name) {
                        already_defined = true;
                        break;
                    }
                }
                if (already_defined) continue; // the derived class overrides this one itself

                Function forward;
                forward.name = derived_method_name;
                forward.loc = base_method.loc;
                forward.return_type = base_method.return_type;
                forward.namespace_path = namespace_path_copy;
                forward.is_exported = is_exported_copy;
                forward.forwards_to = base_method.name;
                forward.body = nullptr; // purely a codegen-level wrapper -- see Function::forwards_to's own comment
                Param this_param;
                this_param.name = "this";
                Type this_type;
                this_type.kind = TypeKind::Reference;
                this_type.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = derived_name});
                this_type.is_mutable_ref = base_method.params[0].type.is_mutable_ref;
                this_param.type = std::move(this_type);
                forward.params.push_back(std::move(this_param));
                for (size_t p = 1; p < base_method.params.size(); p++) {
                    forward.params.push_back(base_method.params[p]);
                }
                program_.functions.push_back(std::move(forward));
            }
        }
    }

    // ch05 §5.14: checks every generic class's own method bodies once,
    // abstractly, at their own definition (ch05 §5.11/§5.14's "checked
    // once at that method's own definition" principle, decomposed per
    // member) -- for each method, substitutes its own constraint's
    // witness (that method's own concept's existing witness class if it
    // has a `requires Concept<T>` clause, or a single, shared,
    // globally-empty "bare witness" struct otherwise -- representing "no
    // operations guaranteed beyond the universal move/store/pass-
    // through/return baseline", ch05 §5.11/§5.14's own words) for the
    // class's own type parameter, both in the method's own signature and
    // throughout its (deep-cloned) body, and in a temporary "checking
    // class" holding the class's own fields substituted the same way.
    // The checking class/method pair is marked so codegen never emits it
    // (ClassDef::is_synthetic_check_only) and so it's checked normally
    // by movecheck but excluded from codegen exactly like an ordinary
    // generic function template (Function::is_generic_template) -- it
    // is never itself reachable from any real call site, existing
    // purely to be type-checked.
    //
    // Deliberately generates one checking class/method pair *per
    // method* rather than trying to detect and reuse ones that happen
    // to share the same witness substitution -- simpler, and the
    // redundant work is harmless (v0.1 doesn't need to be optimal, only
    // correct) -- unlike generic *function* clones (get_or_create_clone),
    // which are cached because they're reachable from arbitrarily many
    // real call sites and would otherwise be duplicated per call.
    //
    // Generic *structs* have no methods to check this way at all (ch04
    // §4.1: no methods, ever) -- their own type parameter is always
    // concept-constrained (parser-enforced), and the only thing that
    // constraint could possibly gate (field triviality) is already
    // re-verified at every concrete instantiation by the ordinary
    // declare_struct check codegen already performs -- nothing here
    // would add anything, so structs are skipped entirely.
    void check_generic_type_methods_once() {
        // Index-based, snapshotting the original class count up front --
        // same reasoning as resolve_generic_types: this loop's own body
        // pushes new entries into program_.classes/program_.functions
        // (once per method), which can reallocate their backing storage.
        // Everything needed from `program_.classes[i]` is copied out
        // into local variables *before* the first push_back in each
        // outer iteration -- a `ClassDef&`/`GenericTypeParam&` reference
        // held across it (as an earlier version of this function did)
        // would silently dangle on the very next inner-loop iteration.
        size_t original_class_count = program_.classes.size();
        for (size_t i = 0; i < original_class_count; i++) {
            if (program_.classes[i].template_params.empty()) continue;
            if (program_.classes[i].is_partial_specialization) continue;
            // ch05 §5.14: a variadic primary template's own bodyless
            // forward declaration, or one of its two fixed
            // specializations, is never itself witness-checked this
            // way -- its "template_params" don't name a single ordinary
            // type parameter the way an the-generic-class-phase-1
            // shape's own does (a pack like "Ts"/"Tail" is never a real
            // type substitutable by a witness at all, and neither
            // variadic shape has ever needed per-method abstract
            // checking so far -- see this class's own constructor
            // comment on variadic_generic_type_names_).
            if (program_.classes[i].is_variadic_primary_template || program_.classes[i].is_variadic_specialization) {
                continue;
            }
            std::vector<GenericTypeParam> template_params = program_.classes[i].template_params;
            std::vector<ClassField> fields_copy = program_.classes[i].fields;
            std::string class_name_copy = program_.classes[i].name;
            std::string owner_id_copy = program_.classes[i].template_owner_id;
            std::vector<Function> methods = method_templates_of_owner(owner_id_copy);
            for (const Function& method_tmpl : methods) {
                if (!method_tmpl.template_params.empty()) continue;
                std::vector<std::pair<std::string, Type>> type_replacements;
                type_replacements.reserve(template_params.size());
                for (size_t param_index = 0; param_index < template_params.size(); ++param_index) {
                    const GenericTypeParam& param = template_params[param_index];
                    if (param.is_non_type) continue;
                    std::string witness_name;
                    if (param_index == 0 && !method_tmpl.method_requires_concept.empty()) {
                        witness_name = method_tmpl.method_requires_concept;
                    } else if (!param.concept_name.empty()) {
                        witness_name = param.concept_name;
                    } else {
                        witness_name = bare_witness_struct_name();
                    }
                    type_replacements.emplace_back(param.name, Type{.kind = TypeKind::Named, .name = witness_name});
                }

                std::string check_class_name = "__genchk" + std::to_string(generic_check_counter_++);
                ClassDef check_class;
                check_class.name = check_class_name;
                check_class.is_synthetic_check_only = true;
                check_class.thread_movable_override = program_.classes[i].thread_movable_override;
                check_class.thread_shareable_override = program_.classes[i].thread_shareable_override;
                if (program_.classes[i].thread_movable_if_movable_expr) {
                    check_class.thread_movable_if_movable_expr = clone_expr(*program_.classes[i].thread_movable_if_movable_expr);
                    substitute_type_params_in_expr(*check_class.thread_movable_if_movable_expr, type_replacements);
                    resolve_generic_types_in_expr(*check_class.thread_movable_if_movable_expr);
                }
                if (program_.classes[i].thread_movable_if_shareable_expr) {
                    check_class.thread_movable_if_shareable_expr = clone_expr(*program_.classes[i].thread_movable_if_shareable_expr);
                    substitute_type_params_in_expr(*check_class.thread_movable_if_shareable_expr, type_replacements);
                    resolve_generic_types_in_expr(*check_class.thread_movable_if_shareable_expr);
                }
                std::unordered_map<std::string, Type> field_types;
                for (const ClassField& f : fields_copy) {
                    ClassField nf;
                    nf.name = f.name;
                    nf.access = f.access;
                    nf.type = substitute_type_params(f.type, type_replacements);
                    field_types[nf.name] = nf.type;
                    check_class.fields.push_back(std::move(nf));
                }
                program_.classes.push_back(std::move(check_class));

                Function check_fn;
                // Keeps the "_methodName" suffix (e.g. "_push"), just
                // against the checking class's own synthesized name
                // instead of the template's -- mirrors ClassName_
                // memberName's own established scheme.
                check_fn.name = check_class_name + method_suffix_after_owner_prefix(method_tmpl, class_name_copy, owner_id_copy);
                check_fn.loc = method_tmpl.loc;
                check_fn.is_generic_template = true;
                check_fn.return_type = substitute_type_params(method_tmpl.return_type, type_replacements);
                check_fn.params.reserve(method_tmpl.params.size());
                for (const Param& p : method_tmpl.params) {
                    Param np;
                    np.name = p.name;
                    if (p.name == "this") {
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = check_class_name});
                        this_type.is_mutable_ref = p.type.is_mutable_ref;
                        np.type = std::move(this_type);
                    } else {
                        np.type = substitute_type_params(p.type, type_replacements);
                    }
                    check_fn.params.push_back(std::move(np));
                }
                check_fn.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (check_fn.body) substitute_type_params_in_stmt(*check_fn.body, type_replacements);
                // ch05 §5.11/§5.14: "calling any method on it or applying
                // any operator to it is a compile error" -- for the
                // *bare* (unconstrained) case specifically (never the
                // concept-constrained one: that witness genuinely has
                // whatever methods its own requires-expression declares,
                // already validated normally by check_moves's ordinary
                // per-function walk over this very check_fn once it's
                // pushed below). The bare witness struct
                // (bare_witness_struct_name) has zero fields/methods by
                // construction, so this can never itself be a false
                // positive -- see reject_calls_on_bare_witness_type's
                // own comment for why check_moves's ordinary call-
                // argument-checking can't already catch this on its own.
                bool uses_bare_witness = false;
                for (const auto& [_, replacement] : type_replacements) {
                    if (replacement.kind == TypeKind::Named && replacement.name == bare_witness_struct_name()) {
                        uses_bare_witness = true;
                        break;
                    }
                }
                if (check_fn.body && uses_bare_witness) {
                    reject_calls_on_bare_witness_type(*check_fn.body, check_class_name, bare_witness_struct_name(),
                                                      field_types);
                }
                program_.functions.push_back(std::move(check_fn));
            }
        }
    }

    // ch05 §5.11/§5.14: recursively walks `stmt` (a synthesized check
    // function's body, check_generic_type_methods_once's own comment)
    // for any Call expression whose receiver -- resolved through a
    // chain of plain Identifier/Member projections, using `field_types`
    // for the one-level "this.field" case and `this_class_name` for
    // `this` itself -- is exactly the bare witness struct
    // (`bare_witness_name`), and throws immediately if one is found: a
    // bare (unconstrained) type parameter guarantees nothing at all, so
    // *any* method call on it is invalid, unconditionally, with no
    // possible false positive (the witness has zero fields/methods by
    // construction). This exists because check_moves's own ordinary
    // call-argument-checking (check_call_arguments) can't reliably catch
    // this on its own: a Member-based receiver's callee resolves to an
    // unmangled, unqualified name it has no way to confirm doesn't exist
    // anywhere else in the whole program (see resolve_callee_signature's
    // own documented scope limitation), and even a precise resolution
    // would find nothing to compare against here anyway, since this
    // synthesized check function is deliberately excluded from codegen
    // (ClassDef::is_synthetic_check_only) -- codegen's own "call to
    // unknown function" check, which every real, compiled method call
    // still gets, never runs for it at all.
    void reject_calls_on_bare_witness_type(const Stmt& stmt, const std::string& this_class_name,
                                            const std::string& bare_witness_name,
                                            const std::unordered_map<std::string, Type>& field_types) {
        // Resolves `expr`'s own type, restricted to exactly the two
        // shapes needed here: a bare `this` (always `this_class_name`)
        // and a single `this.field`/`self.field` projection off it
        // (via `field_types`) -- anything else (a plain local, a
        // deeper chain, a call result, ...) returns nullopt, since a
        // bare type parameter's *only* legal use here is as a field of
        // the generic type's own instance.
        std::function<std::optional<std::string>(const Expr&)> resolve_type_name =
            [&](const Expr& e) -> std::optional<std::string> {
            if (e.kind == ExprKind::Identifier && e.name == "this") return this_class_name;
            if (e.kind == ExprKind::Member) {
                std::optional<std::string> base = resolve_type_name(*e.lhs);
                if (base.has_value() && *base == this_class_name) {
                    auto it = field_types.find(e.name);
                    if (it != field_types.end() && it->second.kind == TypeKind::Named) return it->second.name;
                }
            }
            return std::nullopt;
        };
        std::function<void(const Expr&)> walk_expr = [&](const Expr& e) {
            if (e.kind == ExprKind::Call) {
                if (e.lhs) {
                    std::optional<std::string> receiver_type = resolve_type_name(*e.lhs);
                    if (receiver_type.has_value() && *receiver_type == bare_witness_name) {
                        throw DataflowError(
                            "cannot call method '" + e.name +
                                "' on a value of a bare (unconstrained) generic type parameter -- it guarantees no "
                                "methods at all (spec ch05 §5.11/§5.14); constrain it with a concept to allow this",
                            e.loc);
                    }
                    walk_expr(*e.lhs);
                }
                for (const auto& arg : e.args) walk_expr(*arg);
                return;
            }
            if (e.lhs) walk_expr(*e.lhs);
            if (e.rhs) walk_expr(*e.rhs);
            for (const auto& arg : e.args) walk_expr(*arg);
        };
        std::function<void(const Stmt&)> walk_stmt = [&](const Stmt& s) {
            if (s.init) walk_expr(*s.init);
            for (const auto& arg : s.ctor_args) walk_expr(*arg);
            if (s.expr) walk_expr(*s.expr);
            if (s.condition) walk_expr(*s.condition);
            if (s.then_branch) walk_stmt(*s.then_branch);
            if (s.else_branch) walk_stmt(*s.else_branch);
            for (const auto& inner : s.statements) walk_stmt(*inner);
        };
        walk_stmt(stmt);
    }


    // witness struct standing in for a bare (unconstrained) generic
    // type parameter -- see check_generic_type_methods_once's own
    // comment for why this is a struct (never registered as a "known
    // class" for movecheck's by-value-parameter/no-reassignment
    // restrictions, DataflowState::class_names -- deliberately: a bare
    // type parameter is checked optimistically, as if freely copyable).
    [[nodiscard]] std::string bare_witness_struct_name() {
        if (bare_witness_struct_name_.empty()) {
            bare_witness_struct_name_ = "__generic_bare_witness";
            StructDef witness;
            witness.name = bare_witness_struct_name_;
            program_.structs.push_back(std::move(witness));
        }
        return bare_witness_struct_name_;
    }

    // ch05 §5.14: resolves every not-yet-resolved generic-type
    // instantiation (Type::template_args non-empty) anywhere in the
    // program -- struct/class field types, every function/method's own
    // parameter and return types, and every VarDecl inside a body --
    // synthesizing a concrete instantiation (or reusing an
    // already-cached one, see instantiate_generic_type) and rewriting
    // the Type node's own `name`/clearing `template_args` in place.
    // Struct/class fields and function signatures are resolved first
    // (order doesn't actually matter -- a generic type's own concrete
    // instantiation never depends on anything else being walked -- but
    // this mirrors the natural "declarations before bodies" order), then
    // every function body's own VarDecls.
    void resolve_generic_types() {
        // ch05 §5.14: index-based throughout, snapshotting each original
        // count up front -- resolving one field/parameter/return-type
        // may itself synthesize new struct/class/function entries
        // (instantiate_generic_type), which can reallocate program_.
        // structs/classes/functions' own backing storage. A `Def&`/
        // `Function&` reference held *across* such a call (or even
        // across two separate field accesses straddling one) would
        // silently dangle -- every Type is therefore read into a local
        // copy, resolved by value (resolve_generic_type never mutates
        // through a reference into any of these vectors), and written
        // back via a *fresh* index-based access afterward, never a
        // cached reference spanning the call.
        size_t original_struct_count = program_.structs.size();
        for (size_t i = 0; i < original_struct_count; i++) {
            if (!program_.structs[i].template_params.empty()) continue; // the template itself, never instantiated in place
            size_t field_count = program_.structs[i].fields.size();
            for (size_t j = 0; j < field_count; j++) {
                Type old_type = program_.structs[i].fields[j].type;
                Type new_type = resolve_generic_type(old_type, SourceLocation{});
                program_.structs[i].fields[j].type = new_type;
            }
        }
        size_t original_class_count = program_.classes.size();
        for (size_t i = 0; i < original_class_count; i++) {
            if (!program_.classes[i].template_params.empty()) continue;
            // ch05 §5.14: the empty-pack base-case specialization
            // (`Tuple<>`) is the only variadic ClassDef shape whose own
            // template_params is empty -- it's still a template itself
            // (of the enclosing variadic primary template), never a
            // real, directly-instantiated class, so it's excluded here
            // exactly like every other template's own definition is
            // (see instantiate_variadic_generic_type, which is what
            // actually synthesizes a *concrete* base case).
            if (program_.classes[i].is_variadic_specialization) continue;
            size_t field_count = program_.classes[i].fields.size();
            for (size_t j = 0; j < field_count; j++) {
                Type old_type = program_.classes[i].fields[j].type;
                Type new_type = resolve_generic_type(old_type, SourceLocation{});
                program_.classes[i].fields[j].type = new_type;
            }
            if (program_.classes[i].thread_movable_if_movable_expr) {
                resolve_generic_types_in_expr(*program_.classes[i].thread_movable_if_movable_expr);
            }
            if (program_.classes[i].thread_movable_if_shareable_expr) {
                resolve_generic_types_in_expr(*program_.classes[i].thread_movable_if_shareable_expr);
            }
        }
        size_t original_count = program_.functions.size();
        for (size_t i = 0; i < original_count; i++) {
            if (belongs_to_unresolved_generic_type_template(program_.functions[i])) continue;
            // ch05 §5.11/§5.14: a full-header-form generic *function*'s
            // own template (e.g. `get`/`make`, Function::template_params
            // non-empty) is never resolved here at all -- its own
            // signature may contain a base-class-deduction pattern
            // (`TupleImpl<I, Head, Tail...>& t`) whose "arguments" are
            // only meaningful *symbolically*, referencing this
            // function's own not-yet-bound template parameters, not
            // real concrete types/values resolve_generic_type could
            // make sense of at all. Each concrete call site is instead
            // resolved directly by monomorphize_generic_function_call
            // (mirroring exactly how an abbreviated-Concept-auto-form
            // generic function's own body is similarly left untouched
            // here and only monomorphized per call site).
            if (!program_.functions[i].template_params.empty()) continue;
            SourceLocation loc = program_.functions[i].loc;
            size_t param_count = program_.functions[i].params.size();
            for (size_t j = 0; j < param_count; j++) {
                Type old_type = program_.functions[i].params[j].type;
                Type new_type = resolve_generic_type(old_type, loc);
                program_.functions[i].params[j].type = new_type;
            }
            Type old_return = program_.functions[i].return_type;
            Type new_return = resolve_generic_type(old_return, loc);
            program_.functions[i].return_type = new_return;
            // A function's own body is a stable, independently heap-
            // allocated tree (via StmtPtr) -- never relocated by
            // program_.functions/classes/structs reallocating elsewhere
            // -- so resolving Types *inside* it can safely mutate in
            // place (see resolve_generic_types_in_stmt/_in_expr).
            if (program_.functions[i].body) resolve_generic_types_in_stmt(*program_.functions[i].body);
        }
    }

    void resolve_generic_types_in_stmt(Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                stmt.type = resolve_generic_type(stmt.type, stmt.loc);
                if (stmt.init) resolve_generic_types_in_expr(*stmt.init);
                for (ExprPtr& arg : stmt.ctor_args) resolve_generic_types_in_expr(*arg);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) resolve_generic_types_in_expr(*stmt.expr);
                return;
            case StmtKind::If:
                resolve_generic_types_in_expr(*stmt.condition);
                resolve_generic_types_in_stmt(*stmt.then_branch);
                if (stmt.else_branch) resolve_generic_types_in_stmt(*stmt.else_branch);
                return;
            case StmtKind::While:
                resolve_generic_types_in_expr(*stmt.condition);
                resolve_generic_types_in_stmt(*stmt.then_branch);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) resolve_generic_types_in_stmt(*s);
                return;
        }
    }

    void resolve_generic_types_in_expr(Expr& expr) {
        // MakeUnique's element type / Lambda's explicit return type --
        // safe to mutate directly (see resolve_generic_types' own
        // comment: Expr nodes are stable, independent heap allocations).
        expr.type = resolve_generic_type(expr.type, expr.loc);
        if (expr.lhs) resolve_generic_types_in_expr(*expr.lhs);
        if (expr.rhs) resolve_generic_types_in_expr(*expr.rhs);
        for (ExprPtr& arg : expr.args) resolve_generic_types_in_expr(*arg);
        for (Param& p : expr.lambda_params) p.type = resolve_generic_type(p.type, expr.loc);
        for (LambdaCapture& c : expr.lambda_captures) {
            if (c.init) resolve_generic_types_in_expr(*c.init);
        }
        if (expr.lambda_body) resolve_generic_types_in_stmt(*expr.lambda_body);
        if (expr.kind == ExprKind::TypeTrait) {
            bool value = expr.name == "is_thread_movable" ? is_thread_movable(expr.type) : is_thread_shareable(expr.type);
            expr.kind = ExprKind::BoolLiteral;
            expr.bool_value = value;
            expr.name.clear();
            expr.lhs.reset();
            expr.rhs.reset();
            expr.args.clear();
        }
    }

    // ch05 §5.14: resolves a (possibly not-yet-resolved) generic-type
    // Type value, returning the fully-resolved result *by value* --
    // deliberately never mutating through a reference into
    // program_.functions/classes/structs directly (see this class's
    // other generic-type methods' identical concern): resolving one
    // instantiation may itself append new entries to any of those
    // (instantiate_generic_type), which can reallocate their own
    // backing storage. Operating purely on an owned copy (and, when
    // rebinding pointee/element, a *freshly allocated* shared_ptr rather
    // than mutating through the existing one, which some other, unrelated
    // Type value might still share) sidesteps every such hazard --
    // every caller reads into a local copy and writes the result back
    // via a fresh index-based access afterward, never holding a
    // reference across the call.
    [[nodiscard]] Type resolve_generic_type(Type type, SourceLocation loc) {
        // ch05 §5.14: a variadic generic type (`Tuple<int,bool,char>`,
        // or even the zero-argument `Tuple<>` base case) is checked
        // *before* the ordinary "template_args empty means not a
        // generic instantiation at all" fast path below -- a variadic
        // instantiation's own template_args being empty is itself
        // meaningful (the empty-pack case), unlike an ordinary,
        // non-generic Type (e.g. "int"), which never populates
        // template_args at all. The parser guarantees an *ordinary*
        // (non-variadic) generic type's own template_args is always
        // exactly 1 (see parse_unqualified_type), so this branch can
        // never misfire against one.
        if (variadic_generic_type_names_.contains(type.name)) {
            std::vector<Type> resolved_args;
            resolved_args.reserve(type.template_args.size());
            for (const Type& arg : type.template_args) resolved_args.push_back(resolve_generic_type(arg, loc));
            // ch05 §5.14: this Type's own non_type_args (e.g. the "0" in
            // `TupleImpl<0, int, bool, char>`) are ordinary, self-
            // contained expressions at a top-level use site like this
            // one (never referencing any enclosing template's own
            // parameter -- that symbolic-reference shape only ever
            // appears inside a *generic function's own* deduction-
            // pattern parameter type, which this pass never reaches at
            // all, see run()'s own guard) -- evaluated with an empty
            // parameter-value scope.
            std::vector<int> resolved_non_type_args;
            resolved_non_type_args.reserve(type.non_type_args.size());
            for (const std::shared_ptr<Expr>& arg : type.non_type_args) {
                resolved_non_type_args.push_back(evaluate_non_type_arg(*arg, {}));
            }
            std::string concrete_name =
                instantiate_variadic_generic_type(type.name, resolved_non_type_args, resolved_args, loc);
            type.name = concrete_name;
            type.template_args.clear();
            type.non_type_args.clear();
            return type;
        }
        if (type.template_args.empty()) {
            if (!type.non_type_args.empty()) {
                std::vector<int> resolved_non_type_args;
                resolved_non_type_args.reserve(type.non_type_args.size());
                for (const std::shared_ptr<Expr>& arg : type.non_type_args) {
                    resolved_non_type_args.push_back(evaluate_non_type_arg(*arg, {}));
                }
                std::string concrete_name = instantiate_non_type_generic_type(type.name, resolved_non_type_args, loc);
                type.name = concrete_name;
                type.non_type_args.clear();
                return type;
            }
            if (type.pointee) type.pointee = std::make_shared<Type>(resolve_generic_type(*type.pointee, loc));
            if (type.element) type.element = std::make_shared<Type>(resolve_generic_type(*type.element, loc));
            return type;
        }
        std::vector<Type> resolved_args;
        resolved_args.reserve(type.template_args.size());
        for (const Type& arg : type.template_args) resolved_args.push_back(resolve_generic_type(arg, loc));
        std::string concrete_name = instantiate_generic_type(type.name, resolved_args, loc);
        type.name = concrete_name;
        type.template_args.clear();
        return type;
    }


    // ch05 §5.14: synthesizes (or reuses an already-cached) concrete
    // instantiation of the generic class/struct template named
    // `template_name` for the concrete arguments `concrete_args`, and
    // returns its own mangled name. Validates the template's own
    // class/struct-level concept constraint(s) (if any) against each
    // concrete argument first -- a precise, immediate rejection here,
    // exactly like a generic function's own call-site concept check.
    // For a class template, clones every method whose own
    // `requires Concept<T>` clause (if any) the first concrete argument also
    // satisfies; a method whose own constraint *isn't* satisfied is
    // simply not cloned for this instantiation at all -- calling it
    // surfaces as an ordinary "unknown function" downstream, mirroring
    // the already-accepted precedent for an ungranted operation inside
    // an ordinary generic function's own body (ch05 §5.11) rather than
    // a bespoke "precise diagnostic" message this version doesn't
    // implement.
    [[nodiscard]] std::string instantiate_generic_type(const std::string& template_name,
                                                        const std::vector<Type>& concrete_args,
                                                        SourceLocation loc) {
        std::string cache_key = template_name;
        for (const Type& concrete_arg : concrete_args) {
            cache_key += "." + mangle_type_for_clone_name(concrete_arg);
        }
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        std::vector<Type> named_concretes;
        named_concretes.reserve(concrete_args.size());
        for (const Type& concrete_arg : concrete_args) {
            named_concretes.push_back(concrete_arg.kind == TypeKind::Reference ? *concrete_arg.pointee : concrete_arg);
        }

        // Structs and classes share the same instantiation shape except
        // for AccessSpecifier/method-cloning, so the two are handled
        // by two small, parallel branches rather than one deeply
        // conditional block.
        for (const StructDef& tmpl : program_.structs) {
            if (tmpl.name != template_name || tmpl.template_params.empty()) continue;
            if (tmpl.template_params.size() != named_concretes.size()) {
                throw DataflowError("'" + template_name + "' takes exactly " +
                                        std::to_string(tmpl.template_params.size()) + " template argument(s)",
                                    loc);
            }
            std::vector<std::pair<std::string, Type>> type_replacements;
            type_replacements.reserve(tmpl.template_params.size());
            for (size_t param_index = 0; param_index < tmpl.template_params.size(); ++param_index) {
                const GenericTypeParam& type_param = tmpl.template_params[param_index];
                if (type_param.is_non_type) {
                    throw DataflowError("'" + template_name + "' is not a type-parameter generic class/struct",
                                        loc);
                }
                check_type_param_constraint(type_param, named_concretes[param_index], template_name, loc);
                type_replacements.emplace_back(type_param.name, named_concretes[param_index]);
            }
            StructDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            for (const StructField& f : tmpl.fields) {
                StructField nf;
                nf.name = f.name;
                nf.type = substitute_type_params(f.type, type_replacements);
                nf.type = resolve_generic_type(nf.type, loc);
                concrete.fields.push_back(std::move(nf));
            }
            program_.structs.push_back(std::move(concrete));
            return cache_key;
        }

        OrdinaryClassTemplateSelection class_selection = select_ordinary_class_template(template_name, named_concretes, loc);
        if (class_selection.def != nullptr) {
            const ClassDef& tmpl = *class_selection.def;
            std::string tmpl_owner_id = tmpl.template_owner_id;
            std::vector<std::string> tmpl_namespace_path = tmpl.namespace_path;
            std::string tmpl_base_class_name = tmpl.base_class_name;
            AccessSpecifier tmpl_base_access = tmpl.base_access;
            bool tmpl_thread_movable_override = tmpl.thread_movable_override;
            bool tmpl_thread_shareable_override = tmpl.thread_shareable_override;
            ExprPtr tmpl_thread_movable_if_movable_expr =
                tmpl.thread_movable_if_movable_expr ? clone_expr(*tmpl.thread_movable_if_movable_expr) : nullptr;
            ExprPtr tmpl_thread_movable_if_shareable_expr =
                tmpl.thread_movable_if_shareable_expr ? clone_expr(*tmpl.thread_movable_if_shareable_expr) : nullptr;
            std::vector<GenericTypeParam> template_params_copy = tmpl.template_params;
            std::vector<Function> methods = method_templates_of_owner(tmpl_owner_id);
            for (const GenericTypeParam& type_param : template_params_copy) {
                if (type_param.is_non_type) {
                    throw DataflowError("'" + template_name + "' is not a type-parameter generic class/struct", loc);
                }
                if (type_param.is_pack) {
                    auto pack_it = class_selection.bindings.type_pack_replacements.find(type_param.name);
                    if (pack_it == class_selection.bindings.type_pack_replacements.end()) {
                        throw DataflowError("partial specialization of '" + template_name +
                                                "' did not bind required type pack parameter '" + type_param.name + "'",
                                            loc);
                    }
                    for (const Type& concrete : pack_it->second) {
                        check_type_param_constraint(type_param, concrete, template_name, loc);
                    }
                    continue;
                }
                const Type* bound = find_type_replacement(class_selection.bindings.type_replacements, type_param.name);
                if (bound == nullptr) {
                    throw DataflowError("partial specialization of '" + template_name +
                                            "' did not bind required type parameter '" + type_param.name + "'",
                                        loc);
                }
                check_type_param_constraint(type_param, *bound, template_name, loc);
            }

            std::vector<ClassField> fields_copy = tmpl.fields;
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl_namespace_path;
            concrete.base_class_name = tmpl_base_class_name;
            concrete.base_access = tmpl_base_access;
            concrete.thread_movable_override = tmpl_thread_movable_override;
            concrete.thread_shareable_override = tmpl_thread_shareable_override;
            if (tmpl_thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = std::move(tmpl_thread_movable_if_movable_expr);
                substitute_type_params_in_expr(*concrete.thread_movable_if_movable_expr,
                                               class_selection.bindings.type_replacements);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (tmpl_thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = std::move(tmpl_thread_movable_if_shareable_expr);
                substitute_type_params_in_expr(*concrete.thread_movable_if_shareable_expr,
                                               class_selection.bindings.type_replacements);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            for (const ClassField& f : fields_copy) {
                ClassField nf;
                nf.name = f.name;
                nf.access = f.access;
                nf.type = instantiate_type_pattern(f.type, class_selection.bindings.type_replacements,
                                                   class_selection.bindings.type_pack_replacements);
                nf.type = resolve_generic_type(nf.type, loc);
                concrete.fields.push_back(std::move(nf));
            }
            program_.classes.push_back(std::move(concrete));
            for (const Function& method_tmpl : methods) {
                if (!method_tmpl.method_requires_concept.empty()) {
                    auto concept_it = concepts_by_name_.find(method_tmpl.method_requires_concept);
                    const Type* constrained_type = class_selection.bindings.type_replacements.empty()
                                                       ? nullptr
                                                       : &class_selection.bindings.type_replacements.front().second;
                    bool satisfied = constrained_type != nullptr && concept_it != concepts_by_name_.end() &&
                                      type_satisfies_concept(*constrained_type, *concept_it->second, program_);
                    if (!satisfied) continue;
                }
                Function clone;
                clone.name = cache_key + method_suffix_after_owner_prefix(method_tmpl, template_name, tmpl_owner_id);
                clone.loc = method_tmpl.loc;
                clone.namespace_path = method_tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = method_tmpl.is_unsafe;
                clone.is_generic_template = method_tmpl.is_generic_template;
                clone.template_params = method_tmpl.template_params;
                clone.method_requires_concept = method_tmpl.method_requires_concept;
                clone.return_type = instantiate_type_pattern(method_tmpl.return_type, class_selection.bindings.type_replacements,
                                                             class_selection.bindings.type_pack_replacements);
                clone.return_type = resolve_generic_type(clone.return_type, method_tmpl.loc);
                std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
                clone.params.reserve(method_tmpl.params.size());
                for (const Param& p : method_tmpl.params) {
                    if (p.name == "this") {
                        Param np;
                        np.name = p.name;
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = cache_key});
                        this_type.is_mutable_ref = p.type.is_mutable_ref;
                        np.type = std::move(this_type);
                        clone.params.push_back(std::move(np));
                        continue;
                    }
                    if (p.is_parameter_pack) {
                        std::optional<std::string> pack_name =
                            referenced_type_pack_param_name(p.type, template_params_copy);
                        auto pack_it = pack_name ? class_selection.bindings.type_pack_replacements.find(*pack_name)
                                                 : class_selection.bindings.type_pack_replacements.end();
                        if (pack_name && pack_it != class_selection.bindings.type_pack_replacements.end()) {
                            pack_param_names[p.name] = {};
                            for (size_t j = 0; j < pack_it->second.size(); j++) {
                                Param np = p;
                                np.is_parameter_pack = false;
                                np.name = p.name + "$" + std::to_string(j);
                                std::vector<std::pair<std::string, Type>> param_replacements =
                                    class_selection.bindings.type_replacements;
                                param_replacements.emplace_back(*pack_name, pack_it->second[j]);
                                np.type = instantiate_type_pattern(p.type, param_replacements, {});
                                np.type = resolve_generic_type(np.type, method_tmpl.loc);
                                clone.params.push_back(std::move(np));
                                pack_param_names[p.name].push_back(clone.params.back().name);
                            }
                            continue;
                        }
                    }
                    Param np = p;
                    np.type = instantiate_type_pattern(p.type, class_selection.bindings.type_replacements,
                                                       class_selection.bindings.type_pack_replacements);
                    np.type = resolve_generic_type(np.type, method_tmpl.loc);
                    clone.params.push_back(std::move(np));
                }
                clone.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (clone.body) {
                    substitute_type_params_in_stmt(*clone.body, class_selection.bindings.type_replacements);
                    for (const auto& [class_pack_name, concrete_pack_types] : class_selection.bindings.type_pack_replacements) {
                        std::vector<std::string> concrete_names;
                        concrete_names.reserve(concrete_pack_types.size());
                        for (const Type& concrete_type : concrete_pack_types) concrete_names.push_back(concrete_type.name);
                        expand_explicit_template_arg_packs_in_stmt(*clone.body, class_pack_name, concrete_names);
                    }
                    for (const auto& [pack_param_name, concrete_names] : pack_param_names) {
                        expand_pack_expansions_in_stmt(*clone.body, pack_param_name, concrete_names);
                        expand_pack_folds_in_stmt(*clone.body, pack_param_name, concrete_names);
                    }
                    resolve_generic_types_in_stmt(*clone.body);
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
                            if (!program_.functions.back().template_params.empty()) {
                    generic_template_indices_[program_.functions.back().name] = program_.functions.size() - 1;
                }
            }
            return cache_key;
        }

        throw DataflowError("'" + template_name + "' is not a declared generic type (ch05 §5.14)", loc);
    }

    [[nodiscard]] std::string instantiate_non_type_generic_type(const std::string& template_name,
                                                                const std::vector<int>& non_type_args,
                                                                SourceLocation loc) {
        std::string cache_key = template_name;
        for (int value : non_type_args) cache_key += "." + std::to_string(value);
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        for (const StructDef& tmpl : program_.structs) {
            if (tmpl.name != template_name || tmpl.template_params.size() != non_type_args.size() ||
                tmpl.template_params.empty() || !tmpl.template_params[0].is_non_type) {
                continue;
            }
            StructDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            concrete.fields = tmpl.fields;
            program_.structs.push_back(std::move(concrete));
            return cache_key;
        }

        for (const ClassDef& tmpl : program_.classes) {
            if (tmpl.name != template_name || tmpl.template_params.size() != non_type_args.size() ||
                tmpl.template_params.empty() || !tmpl.template_params[0].is_non_type) {
                continue;
            }
            std::vector<GenericTypeParam> params_copy = tmpl.template_params;
            std::string owner_id_copy = tmpl.template_owner_id;
            std::vector<ClassField> fields_copy = tmpl.fields;
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = tmpl.namespace_path;
            concrete.thread_movable_override = tmpl.thread_movable_override;
            concrete.thread_shareable_override = tmpl.thread_shareable_override;
            if (tmpl.thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = clone_expr(*tmpl.thread_movable_if_movable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (tmpl.thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = clone_expr(*tmpl.thread_movable_if_shareable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            for (const ClassField& field : fields_copy) concrete.fields.push_back(field);
            program_.classes.push_back(std::move(concrete));

            std::vector<Function> methods = method_templates_of_owner(owner_id_copy);
            for (const Function& method_tmpl : methods) {
                Function clone;
                clone.name = cache_key + method_suffix_after_owner_prefix(method_tmpl, template_name, owner_id_copy);
                clone.loc = method_tmpl.loc;
                clone.namespace_path = method_tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = method_tmpl.is_unsafe;
                clone.is_generic_template = method_tmpl.is_generic_template;
                clone.template_params = method_tmpl.template_params;
                clone.method_requires_concept = method_tmpl.method_requires_concept;
                clone.return_type = method_tmpl.return_type;
                clone.params.reserve(method_tmpl.params.size());
                for (const Param& param : method_tmpl.params) {
                    Param new_param;
                    new_param.name = param.name;
                    if (param.name == "this") {
                        Type this_type;
                        this_type.kind = TypeKind::Reference;
                        this_type.pointee = std::make_shared<Type>(Type{.kind = TypeKind::Named, .name = cache_key});
                        this_type.is_mutable_ref = param.type.is_mutable_ref;
                        new_param.type = std::move(this_type);
                    } else {
                        new_param.type = param.type;
                    }
                    clone.params.push_back(std::move(new_param));
                }
                clone.body = method_tmpl.body ? clone_stmt(*method_tmpl.body) : nullptr;
                if (clone.body) {
                    for (size_t i = 0; i < params_copy.size(); i++) {
                        substitute_non_type_param_in_stmt(*clone.body, params_copy[i].name, non_type_args[i]);
                    }
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
                if (!program_.functions.back().template_params.empty()) {
                    generic_template_indices_[program_.functions.back().name] = program_.functions.size() - 1;
                }
            }
            return cache_key;
        }

        throw DataflowError("'" + template_name + "' is not a declared generic type (ch05 §5.14)", loc);
    }

    // ch05 §5.14: synthesizes (or reuses an already-cached) concrete
    // instantiation of a variadic generic type's own recursive-
    // inheritance chain -- one concrete ClassDef per level, from
    // `type_args[0]` down to the terminal empty-pack base case, each
    // level's own base_class_name pointing at the next level's own
    // synthesized name (mirroring exactly how the doc's own
    // `Tuple<Head, Tail...> : private Tuple<Tail...>` recursive
    // specialization is meant to expand). `non_type_args` holds every
    // *leading* non-type argument's own already-evaluated concrete
    // value (e.g. TupleImpl's own "Idx" -- empty for a primary template
    // with no non-type parameter at all, like plain Tuple). Returns the
    // *outermost* (fullest) level's own mangled name -- what
    // `TupleImpl<0,int,bool,char>` itself resolves to. Neither of the
    // doc's own two variadic examples (Tuple, TupleImpl) ever defines a
    // method on a variadic specialization, so unlike
    // instantiate_generic_type's class branch, no method-cloning
    // happens here at all -- see method_templates_of's own comment:
    // every specialization sharing the same `name` would be
    // indistinguishable by a `this`-type-pointee-name scan alone, so
    // naively reusing it here would be unsound (a known, deliberately
    // out-of-scope gap for now).
    [[nodiscard]] std::string instantiate_variadic_generic_type(const std::string& template_name,
                                                                 const std::vector<int>& non_type_args,
                                                                 const std::vector<Type>& type_args,
                                                                 SourceLocation loc) {
        std::string cache_key = template_name;
        for (int v : non_type_args) cache_key += "." + std::to_string(v);
        cache_key += type_args.empty() ? ".empty" : "";
        for (const Type& arg : type_args) cache_key += "." + mangle_type_for_clone_name(arg);
        auto cached = generic_type_instance_cache_.find(cache_key);
        if (cached != generic_type_instance_cache_.end()) return cached->second;
        generic_type_instance_cache_[cache_key] = cache_key;

        if (type_args.empty()) {
            // The empty-pack base case: `template<> class Tuple<>
            // { ... };`, or (with a leading non-type parameter, e.g.
            // TupleImpl) `template<int Idx> class TupleImpl<Idx>
            // { ... };` -- must already be declared (parser-enforced
            // for every variadic primary template that's ever
            // specialized at all, but not necessarily reached by any
            // *use* of the recursive case -- a `TupleImpl<0,int>`
            // instantiation still needs to bottom out at TupleImpl<1>'s
            // own concrete instance one level down).
            const ClassDef* base_case_tmpl = nullptr;
            for (const ClassDef& c : program_.classes) {
                if (c.name == template_name && c.is_variadic_specialization &&
                    c.template_params.size() == non_type_args.size()) {
                    base_case_tmpl = &c;
                    break;
                }
            }
            if (!base_case_tmpl) {
                throw DataflowError("'" + template_name + "' has no declared empty-pack base-case specialization "
                                                            "matching " +
                                         std::to_string(non_type_args.size()) + " non-type argument(s) (ch05 §5.14)",
                    loc);
            }
            ClassDef concrete;
            concrete.name = cache_key;
            concrete.namespace_path = base_case_tmpl->namespace_path;
            concrete.thread_movable_override = base_case_tmpl->thread_movable_override;
            concrete.thread_shareable_override = base_case_tmpl->thread_shareable_override;
            if (base_case_tmpl->thread_movable_if_movable_expr) {
                concrete.thread_movable_if_movable_expr = clone_expr(*base_case_tmpl->thread_movable_if_movable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
            }
            if (base_case_tmpl->thread_movable_if_shareable_expr) {
                concrete.thread_movable_if_shareable_expr = clone_expr(*base_case_tmpl->thread_movable_if_shareable_expr);
                resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
            }
            program_.classes.push_back(std::move(concrete));
            variadic_instance_info_[cache_key] = VariadicInstanceInfo{template_name, non_type_args, type_args};
            return cache_key;
        }

        const ClassDef* recursive_tmpl = nullptr;
        for (const ClassDef& c : program_.classes) {
            if (c.name == template_name && c.is_variadic_specialization &&
                c.template_params.size() == non_type_args.size() + 2 &&
                c.template_params[non_type_args.size()].is_pack == false &&
                c.template_params[non_type_args.size() + 1].is_pack) {
                recursive_tmpl = &c;
                break;
            }
        }
        if (!recursive_tmpl) {
            throw DataflowError("'" + template_name + "' has no declared recursive-case specialization to match " +
                                     std::to_string(non_type_args.size()) + " non-type and " +
                                     std::to_string(type_args.size()) + " type argument(s) (ch05 §5.14)",
                loc);
        }

        // Copy everything needed out of `recursive_tmpl` *before* the
        // recursive instantiate_variadic_generic_type call below,
        // which pushes a new ClassDef into program_.classes and can
        // reallocate its backing storage -- `recursive_tmpl` itself
        // (a bare pointer into that vector) would otherwise dangle
        // (see this class's other generic-type methods' identical
        // concern).
        size_t leading_non_type_count = non_type_args.size();
        std::vector<GenericTypeParam> leading_non_type_params(
            recursive_tmpl->template_params.begin(), recursive_tmpl->template_params.begin() + leading_non_type_count);
        GenericTypeParam head_param = recursive_tmpl->template_params[leading_non_type_count];
        std::string base_template_name = recursive_tmpl->base_class_name;
        AccessSpecifier base_access = recursive_tmpl->base_access;
        std::vector<ClassField> fields_copy = recursive_tmpl->fields;
        std::vector<std::string> namespace_path_copy = recursive_tmpl->namespace_path;
        std::shared_ptr<Expr> base_non_type_arg_expr = recursive_tmpl->base_non_type_arg;

        Type head_concrete = type_args[0];
        std::vector<Type> tail_concrete(type_args.begin() + 1, type_args.end());
        check_type_param_constraint(head_param, head_concrete, template_name, loc);

        // ch05 §5.14: the base's own non-type argument (e.g. "Idx + 1"
        // in TupleImpl's own `: public TupleImpl<Idx + 1, Tail...>`) is
        // evaluated using *this* level's own non-type parameter values
        // (e.g. this level's own concrete "Idx") -- empty when the base
        // template has no non-type parameter at all (plain Tuple's own
        // `: private Tuple<Tail...>`).
        std::vector<int> base_non_type_args;
        if (base_non_type_arg_expr) {
            std::unordered_map<std::string, int> param_values;
            for (size_t i = 0; i < leading_non_type_params.size(); i++) {
                param_values[leading_non_type_params[i].name] = non_type_args[i];
            }
            base_non_type_args.push_back(evaluate_non_type_arg(*base_non_type_arg_expr, param_values));
        }

        std::string base_concrete_name =
            instantiate_variadic_generic_type(base_template_name, base_non_type_args, tail_concrete, loc);

        ClassDef concrete;
        concrete.name = cache_key;
        concrete.namespace_path = namespace_path_copy;
        concrete.base_class_name = base_concrete_name;
        concrete.base_access = base_access;
        concrete.thread_movable_override = recursive_tmpl->thread_movable_override;
        concrete.thread_shareable_override = recursive_tmpl->thread_shareable_override;
        if (recursive_tmpl->thread_movable_if_movable_expr) {
            concrete.thread_movable_if_movable_expr = clone_expr(*recursive_tmpl->thread_movable_if_movable_expr);
            substitute_type_param_in_expr(*concrete.thread_movable_if_movable_expr, head_param.name, head_concrete);
            resolve_generic_types_in_expr(*concrete.thread_movable_if_movable_expr);
        }
        if (recursive_tmpl->thread_movable_if_shareable_expr) {
            concrete.thread_movable_if_shareable_expr = clone_expr(*recursive_tmpl->thread_movable_if_shareable_expr);
            substitute_type_param_in_expr(*concrete.thread_movable_if_shareable_expr, head_param.name, head_concrete);
            resolve_generic_types_in_expr(*concrete.thread_movable_if_shareable_expr);
        }
        for (const ClassField& f : fields_copy) {
            ClassField nf;
            nf.name = f.name;
            nf.access = f.access;
            nf.type = substitute_type_param(f.type, head_param.name, head_concrete);
            concrete.fields.push_back(std::move(nf));
        }
        program_.classes.push_back(std::move(concrete));
        variadic_instance_info_[cache_key] = VariadicInstanceInfo{template_name, non_type_args, type_args};
        return cache_key;
    }

    // ch05 §5.14: evaluates a variadic generic type's own non-type
    // argument expression down to a concrete int -- restricted to a
    // small, purpose-scoped shape (an integer literal; a bare
    // identifier, looked up in `param_values`, e.g. an enclosing
    // specialization's own non-type parameter name; or a `+` of the
    // two, e.g. "Idx + 1"), not a general compile-time constant-
    // expression evaluator (ch05 §5.14's own scoping: non-type
    // parameters are a narrow, purpose-built feature for a variadic
    // type's own recursive indexing, not a general `consteval`
    // mechanism). Throws a precise DataflowError for any other shape,
    // or an identifier not found in `param_values`.
    [[nodiscard]] int evaluate_non_type_arg(const Expr& expr, const std::unordered_map<std::string, int>& param_values) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return static_cast<int>(expr.int_value);
            case ExprKind::Identifier: {
                auto it = param_values.find(expr.name);
                if (it == param_values.end()) {
                    throw DataflowError("'" + expr.name +
                                         "' does not name a known non-type template parameter here (ch05 §5.14)",
                        expr.loc);
                }
                return it->second;
            }
            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Add) {
                    return evaluate_non_type_arg(*expr.lhs, param_values) +
                           evaluate_non_type_arg(*expr.rhs, param_values);
                }
                [[fallthrough]];
            default:
                throw DataflowError("unsupported non-type template argument expression (ch05 §5.14 only supports "
                                     "an integer literal, a bare parameter name, or a '+' of the two)",
                    expr.loc);
        }
    }

    // ch05 §5.14: given `pattern` (a generic function's own base-class-
    // deduction parameter type, e.g. `TupleImpl<I, Head, Tail...>`,
    // still bearing its own *symbolic* template_args/non_type_args
    // referencing the enclosing function template's own parameter
    // names) and the actual call argument at `arg_index`, walks that
    // argument's own concrete type's base-class chain (via
    // variadic_instance_info_, populated by instantiate_variadic_
    // generic_type) looking for the unique level whose own recorded
    // non-type value(s) match `pattern`'s own (by-now-substituted-with-
    // already-bound-values) non-type arguments -- real, standard C++
    // template-argument deduction from a base class ([temp.deduct.call]),
    // not a scpp-specific mechanism (see ch05 §5.14's own doc comment).
    // Binds every one of `pattern`'s own type-parameter-position
    // symbolic references (e.g. "Head") to that matched level's own
    // concrete type argument, and records an upcast for `arg_index`
    // (its actual argument needs to be treated as the matched level's
    // own, less-derived concrete type when calling the monomorphized
    // clone -- codegen needs no actual conversion instruction for this,
    // since every level's own flattened layout is already byte-
    // compatible with its base, see ClassDef::base_class_name's own
    // comment; this is purely a scpp-level type-compatibility fact).
    void deduce_via_base_class_chain(const Expr& expr, size_t arg_index, const Type& pattern, Body& body,
                                      std::unordered_map<std::string, Type>& type_bindings,
                                      std::unordered_map<std::string, int>& value_bindings,
                                      std::vector<std::pair<size_t, Type>>& upcasts) {
        std::vector<int> search_non_type_values;
        search_non_type_values.reserve(pattern.non_type_args.size());
        for (const std::shared_ptr<Expr>& e : pattern.non_type_args) {
            search_non_type_values.push_back(evaluate_non_type_arg(*e, value_bindings));
        }

        std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_index], body, signatures_);
        if (!arg_type.has_value()) {
            throw DataflowError("cannot resolve the type of this argument for base-class deduction (ch05 §5.14)",
                expr.loc);
        }
        Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;

        std::string current_name = named.name;
        const VariadicInstanceInfo* matched = nullptr;
        std::string matched_name;
        while (true) {
            auto it = variadic_instance_info_.find(current_name);
            if (it == variadic_instance_info_.end()) break;
            if (it->second.template_name == pattern.name && it->second.non_type_values == search_non_type_values) {
                matched = &it->second;
                matched_name = current_name;
                break;
            }
            const ClassDef* cd = nullptr;
            for (const ClassDef& c : program_.classes) {
                if (c.name == current_name) {
                    cd = &c;
                    break;
                }
            }
            if (!cd || cd->base_class_name.empty()) break;
            current_name = cd->base_class_name;
        }
        if (!matched) {
            throw DataflowError("no base class (direct or indirect) of the argument's own type matches the "
                                 "pattern '" +
                                     pattern.name + "<...>' (ch05 §5.14 base-class deduction)",
                expr.loc);
        }

        size_t ti = 0;
        for (const Type& sym : pattern.template_args) {
            if (sym.is_pack_expansion) break; // the doc's own examples never use the pack itself directly
            if (ti < matched->type_args.size()) {
                type_bindings[sym.name] = matched->type_args[ti];
                ti++;
            }
        }

        Type target;
        target.kind = TypeKind::Named;
        target.name = matched_name;
        upcasts.emplace_back(arg_index, std::move(target));
    }

    // ch05 §5.15: is `name` one of the scalar type names this version
    // actually implements? (Only `int`/`bool`/`char` exist as real scpp
    // types so far -- see this file's own earlier notes on `size_t`/
    // fixed-width integers/`float32_t`/`float64_t` not existing yet;
    // every scalar is both thread-movable and thread-shareable.)
    [[nodiscard]] static bool is_scalar_type_name(const std::string& name) {
        return name == "int" || name == "bool" || name == "char";
    }

    // ch05 §5.15: recursively computes whether `type` is thread-movable
    // (mirrors Rust's `Send`) -- see this document's own §5.15 for the
    // full structural-derivation rules. `visiting` guards against
    // infinite recursion through a self-referential type (e.g. `class
    // Node { std::unique_ptr<Node> next; };`, a realistic linked-list
    // shape) -- coinductively assumed thread-movable the moment a cycle
    // is detected (the recursive occurrence contributes no *new*
    // violation beyond whatever the rest of the type's own fields
    // already determine), mirroring how a real compiler's own auto-trait
    // computation (e.g. Rust's `Send`/`Sync` auto-derivation) handles a
    // recursive type without looping forever.
    [[nodiscard]] bool evaluate_thread_bool_constant_expr(const Expr& expr, std::unordered_set<std::string> visiting = {}) {
        switch (expr.kind) {
            case ExprKind::BoolLiteral:
                return expr.bool_value;
            case ExprKind::TypeTrait:
                return expr.name == "is_thread_movable" ? is_thread_movable(expr.type, visiting)
                                                        : is_thread_shareable(expr.type, visiting);
            case ExprKind::Unary:
                if (expr.unary_op == UnaryOp::Not && expr.lhs) {
                    return !evaluate_thread_bool_constant_expr(*expr.lhs, visiting);
                }
                break;
            case ExprKind::Binary:
                if (!expr.lhs || !expr.rhs) break;
                if (expr.binary_op == BinaryOp::And) {
                    return evaluate_thread_bool_constant_expr(*expr.lhs, visiting) &&
                           evaluate_thread_bool_constant_expr(*expr.rhs, visiting);
                }
                if (expr.binary_op == BinaryOp::Or) {
                    return evaluate_thread_bool_constant_expr(*expr.lhs, visiting) ||
                           evaluate_thread_bool_constant_expr(*expr.rhs, visiting);
                }
                if (expr.binary_op == BinaryOp::Eq) {
                    return evaluate_thread_bool_constant_expr(*expr.lhs, visiting) ==
                           evaluate_thread_bool_constant_expr(*expr.rhs, visiting);
                }
                if (expr.binary_op == BinaryOp::Ne) {
                    return evaluate_thread_bool_constant_expr(*expr.lhs, visiting) !=
                           evaluate_thread_bool_constant_expr(*expr.rhs, visiting);
                }
                break;
            default:
                break;
        }
        throw DataflowError("thread-trait override expressions must be boolean constant expressions built from "
                            "bool literals, !, &&, ||, ==, !=, and scpp::is_thread_movable/shareable(T)",
                            expr.loc);
    }

    [[nodiscard]] bool is_thread_movable(const Type& type, std::unordered_set<std::string> visiting = {}) {
        switch (type.kind) {
            case TypeKind::Named: {
                if (is_scalar_type_name(type.name)) return true;
                if (visiting.contains(type.name)) return true; // cycle -- see this function's own comment
                visiting.insert(type.name);
                for (const ClassDef& c : program_.classes) {
                    if (c.name != type.name) continue;
                    if (c.thread_movable_override) return true;
                    if (c.thread_movable_if_movable_expr) {
                        return evaluate_thread_bool_constant_expr(*c.thread_movable_if_movable_expr, visiting);
                    }
                    // ch05 §5.15: a `mutable` member variable (ch04
                    // §4.2's own interior-mutability construct) would
                    // never itself defeat thread-movable (it's always a
                    // trivial scalar, already true structurally) -- no
                    // special-casing needed here at all, only for
                    // thread-shareable below. `mutable` fields aren't
                    // actually implemented in this version yet regardless
                    // (see ClassField's own comment), so this is currently
                    // moot either way.
                    for (const ClassField& f : c.fields) {
                        if (!is_thread_movable(f.type, visiting)) return false;
                    }
                    return true;
                }
                for (const StructDef& s : program_.structs) {
                    if (s.name != type.name) continue;
                    if (s.thread_movable_override) return true;
                    for (const StructField& f : s.fields) {
                        if (!is_thread_movable(f.type, visiting)) return false;
                    }
                    return true;
                }
                return false; // unrecognized (e.g. a bare, unconstrained generic witness) -- conservative default
            }
            case TypeKind::Pointer:
                // ch05 §5.15: neither, by default -- a raw pointer
                // already requires vouching for anything the checker
                // can't verify on its own; vouch by wrapping it in a
                // struct/class marked `[[scpp::thread_movable]]`, whose
                // Named case above already handles the override
                // directly (this Pointer case itself never recurses
                // into the pointee, since a raw pointer's own structural
                // shape carries no ownership/borrow information at all).
                return false;
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                return true;
            case TypeKind::Array: return type.element && is_thread_movable(*type.element, visiting);
            case TypeKind::Reference:
                // ch05 §5.15: "a type containing a reference member...
                // is never thread-movable" -- applied here to a bare
                // *borrowed* reference value (`T&`/`const T&`), for the
                // identical reason (moving a reference never transfers
                // the referent's own ownership, so the original thread
                // keeps access to it regardless). An *rvalue* reference
                // (`T&&`) is a fundamentally different case not
                // addressed by that rule at all -- it represents a
                // move (ownership transfer, ch03), not a borrow, so its
                // own thread-movable-ness instead follows its
                // underlying type's, exactly like std::unique_ptr's own
                // pointee does above (the doc's own examples never spell
                // this case out explicitly, but `spawn`'s own
                // `T&& f [[scpp::thread_movable]]` parameter, ch05
                // §5.15's own worked example, only makes sense at all if
                // an rvalue reference to a movable type is itself
                // considered thread-movable).
                return type.is_rvalue_ref ? (type.pointee && is_thread_movable(*type.pointee, visiting)) : false;
            case TypeKind::Span:
                // A span is a non-owning, lifetime-checked borrowed view
                // -- the same "never transfers ownership" reasoning as
                // an ordinary (non-rvalue) Reference above applies
                // identically; a span is never an rvalue-reference-like
                // move construct at all.
                return false;
        }
        return false;
    }

    // ch05 §5.15: recursively computes whether `type` is thread-
    // shareable (mirrors Rust's `Sync`) -- see is_thread_movable's own
    // comment for the identical cycle-guarding approach.
    [[nodiscard]] bool is_thread_shareable(const Type& type, std::unordered_set<std::string> visiting = {}) {
        switch (type.kind) {
            case TypeKind::Named: {
                if (is_scalar_type_name(type.name)) return true;
                if (visiting.contains(type.name)) return true;
                visiting.insert(type.name);
                for (const ClassDef& c : program_.classes) {
                    if (c.name != type.name) continue;
                    if (c.thread_shareable_override) return true;
                    if (c.thread_movable_if_shareable_expr) {
                        return evaluate_thread_bool_constant_expr(*c.thread_movable_if_shareable_expr, visiting);
                    }
                    for (const ClassField& f : c.fields) {
                        if (!is_thread_shareable(f.type, visiting)) return false;
                    }
                    return true;
                }
                for (const StructDef& s : program_.structs) {
                    if (s.name != type.name) continue;
                    if (s.thread_shareable_override) return true;
                    for (const StructField& f : s.fields) {
                        if (!is_thread_shareable(f.type, visiting)) return false;
                    }
                    return true;
                }
                return false;
            }
            case TypeKind::Pointer: return false;
            case TypeKind::Function:
            case TypeKind::FunctionPointer: return true;
            case TypeKind::Array: return type.element && is_thread_shareable(*type.element, visiting);
            case TypeKind::Reference:
                // ch05 §5.15: an *rvalue* reference (`T&&`) represents a
                // move, not a borrow (see is_thread_movable's own,
                // identical reasoning) -- follows the underlying type's
                // own thread-shareable-ness directly, bypassing the
                // mutable/const-borrow distinction below entirely (an
                // rvalue reference is neither).
                if (type.is_rvalue_ref) return type.pointee && is_thread_shareable(*type.pointee, visiting);
                // A *mutable* (borrowed) reference is never thread-
                // shareable -- two threads simultaneously holding
                // `const ThisType&` to a container of one, both able to
                // reach through and write the same referent
                // concurrently, is exactly the race thread-shareable
                // exists to rule out (this is the same reasoning
                // §5.15's own closure-capture bullet states explicitly,
                // applied uniformly to any mutable-reference-typed
                // value, not just a closure's own capture). A *shared*
                // (`const`) reference is thread-shareable exactly when
                // its own referent type is (matches §5.15's own
                // closure-capture bullet too: "every by-const-reference-
                // captured member's referent type is thread-shareable").
                return type.pointee && !type.is_mutable_ref && is_thread_shareable(*type.pointee, visiting);
            case TypeKind::Span:
                return type.pointee && !type.is_mutable_ref && is_thread_shareable(*type.pointee, visiting);
        }
        return false;
    }

    // ch05 §5.15: checks a call to a generic function whose own
    // parameter is tagged `[[scpp::thread_movable]]`/
    // `[[scpp::thread_shareable]]` (Param::require_thread_movable/
    // require_thread_shareable) -- once that parameter's own
    // (possibly template-deduced) concrete type is known, rejects the
    // call with a precise diagnostic if the concrete type doesn't
    // actually satisfy the required property.
    void check_thread_safety_constraints(const Expr& expr, const Function& tmpl,
                                          const std::unordered_map<std::string, Type>& type_bindings) {
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (!param.require_thread_movable && !param.require_thread_shareable) continue;
            Type concrete = param.type;
            for (const auto& [name, replacement] : type_bindings) {
                concrete = substitute_type_param(concrete, name, replacement);
            }
            if (param.require_thread_movable && !is_thread_movable(concrete)) {
                throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                         tmpl.name +
                                         "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                    expr.loc);
            }
            if (param.require_thread_shareable && !is_thread_shareable(concrete)) {
                throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                         tmpl.name +
                                         "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                    expr.loc);
            }
        }
    }

    void maybe_instantiate_generic_constructor_overloads(const std::string& class_name,
                                                          const std::vector<ExprPtr>& args, Body& body,
                                                          SourceLocation loc) {
        std::string ctor_name = class_name + "_new";
        for (const Function& tmpl : program_.functions) {
            if (tmpl.name != ctor_name || tmpl.template_params.empty()) continue;
            try {
                std::unordered_map<std::string, Type> type_bindings;
                std::unordered_map<std::string, int> value_bindings;
                std::vector<std::pair<size_t, Type>> upcasts;
                std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());

                size_t arg_cursor = 0;
                for (size_t i = 1; i < tmpl.params.size() && arg_cursor < args.size(); i++) {
                    if (tmpl.params[i].is_parameter_pack) {
                        std::optional<std::string> pack_type_name =
                            referenced_type_pack_param_name(tmpl.params[i].type, tmpl.template_params);
                        for (; arg_cursor < args.size(); arg_cursor++) {
                            std::optional<Type> arg_type = infer_expr_type(*args[arg_cursor], body, signatures_);
                            if (!arg_type.has_value()) continue;
                            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                            Type substituted = pack_type_name.has_value()
                                                   ? substitute_type_param(tmpl.params[i].type, *pack_type_name, named)
                                                   : tmpl.params[i].type;
                            concrete_pack_param_types[i].push_back(std::move(substituted));
                        }
                        continue;
                    }
                    const Type& param_type = tmpl.params[i].type;
                    const Type& underlying = param_type.kind == TypeKind::Reference ? *param_type.pointee : param_type;
                    if (underlying.kind != TypeKind::Named) {
                        arg_cursor++;
                        continue;
                    }
                    if (underlying.template_args.empty() && underlying.non_type_args.empty()) {
                        for (const GenericTypeParam& tp : tmpl.template_params) {
                            if (tp.is_non_type || tp.name != underlying.name || type_bindings.contains(tp.name)) continue;
                            std::optional<Type> arg_type = infer_expr_type(*args[arg_cursor], body, signatures_);
                            if (!arg_type.has_value()) continue;
                            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                            type_bindings[tp.name] = named;
                        }
                        arg_cursor++;
                        continue;
                    }
                    if (variadic_generic_type_names_.contains(underlying.name)) {
                        Expr fake_call;
                        fake_call.loc = loc;
                        for (const ExprPtr& arg : args) fake_call.args.push_back(clone_expr(*arg));
                        deduce_via_base_class_chain(fake_call, arg_cursor, underlying, body, type_bindings, value_bindings,
                                                    upcasts);
                    }
                    arg_cursor++;
                }

                for (const GenericTypeParam& tp : tmpl.template_params) {
                    if (tp.is_pack) continue;
                    bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
                    if (!bound) throw DataflowError("constructor template parameter not deduced", loc);
                }

                Expr fake_call;
                fake_call.loc = loc;
                check_thread_safety_constraints(fake_call, tmpl, type_bindings);

                std::string cache_key = tmpl.name;
                for (const GenericTypeParam& tp : tmpl.template_params) {
                    if (tp.is_pack) continue;
                    cache_key += tp.is_non_type ? ("." + std::to_string(value_bindings[tp.name]))
                                                : ("." + mangle_type_for_clone_name(type_bindings[tp.name]));
                }
                for (size_t i = 0; i < tmpl.params.size(); i++) {
                    if (!tmpl.params[i].is_parameter_pack) continue;
                    for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
                }
                if (generic_function_clone_cache_.contains(cache_key)) continue;
                generic_function_clone_cache_[cache_key] = tmpl.name;

                Function clone;
                std::string concrete_ctor_owner_name = class_name;
                if (std::optional<Type> this_type = this_type_of(tmpl)) concrete_ctor_owner_name = this_type->name;
                clone.name = class_name +
                             method_suffix_after_owner_prefix(tmpl, concrete_ctor_owner_name, tmpl.generic_method_owner_id);
                clone.loc = tmpl.loc;
                clone.namespace_path = tmpl.namespace_path;
                clone.is_exported = false;
                clone.is_unsafe = tmpl.is_unsafe;
                clone.return_type = tmpl.return_type;
                for (const auto& [name, replacement] : type_bindings) {
                    clone.return_type = substitute_type_param(clone.return_type, name, replacement);
                }
                clone.return_type = resolve_generic_type(clone.return_type, tmpl.loc);
                clone.params.reserve(tmpl.params.size());
                std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
                for (size_t i = 0; i < tmpl.params.size(); i++) {
                    if (tmpl.params[i].is_parameter_pack) {
                        pack_param_names[tmpl.params[i].name] = {};
                        for (size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                            Param p;
                            p.name = tmpl.params[i].name + "$" + std::to_string(j);
                            p.type = concrete_pack_param_types[i][j];
                            p.require_thread_movable = tmpl.params[i].require_thread_movable;
                            p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                            clone.params.push_back(std::move(p));
                            pack_param_names[tmpl.params[i].name].push_back(tmpl.params[i].name + "$" +
                                                                            std::to_string(j));
                        }
                        continue;
                    }
                    Param p;
                    p.name = tmpl.params[i].name;
                    p.type = tmpl.params[i].type;
                    p.require_thread_movable = tmpl.params[i].require_thread_movable;
                    p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                    bool upcasted = false;
                    for (const auto& [idx, target] : upcasts) {
                        if (idx != i) continue;
                        if (p.type.kind == TypeKind::Reference) {
                            p.type.pointee = std::make_shared<Type>(target);
                        } else {
                            p.type = target;
                        }
                        upcasted = true;
                        break;
                    }
                    if (!upcasted) {
                        for (const auto& [name, replacement] : type_bindings) {
                            p.type = substitute_type_param(p.type, name, replacement);
                        }
                    }
                    p.type = resolve_generic_type(p.type, tmpl.loc);
                    clone.params.push_back(std::move(p));
                }
                clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
                if (clone.body) {
                    for (const auto& [name, replacement] : type_bindings) {
                        substitute_type_param_in_stmt(*clone.body, name, replacement);
                    }
                    for (const auto& [pack_name, concrete_names] : pack_param_names) {
                        expand_pack_expansions_in_stmt(*clone.body, pack_name, concrete_names);
                        expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
                    }
                    resolve_generic_types_in_stmt(*clone.body);
                }
                known_function_names_.insert(clone.name);
                program_.functions.push_back(std::move(clone));
                walk_new_concrete_function(program_.functions.size() - 1);
            } catch (const DataflowError&) {
                continue;
            }
        }
    }

    std::string instantiate_full_header_generic_clone(const Function& tmpl,
                                                      const std::unordered_map<std::string, Type>& type_bindings,
                                                      const std::unordered_map<std::string, int>& value_bindings,
                                                      const std::vector<std::vector<Type>>& concrete_pack_param_types,
                                                      const std::vector<std::pair<size_t, Type>>& upcasts = {}) {
        std::string cache_key = tmpl.name;
        for (const GenericTypeParam& tp : tmpl.template_params) {
            if (tp.is_pack) continue;
            cache_key += tp.is_non_type ? ("." + std::to_string(value_bindings.at(tp.name)))
                                        : ("." + mangle_type_for_clone_name(type_bindings.at(tp.name)));
        }
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            if (!tmpl.params[i].is_parameter_pack) continue;
            for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
        }
        auto cached = generic_function_clone_cache_.find(cache_key);
        if (cached != generic_function_clone_cache_.end()) return cached->second;
        generic_function_clone_cache_[cache_key] = cache_key;

        Function clone;
        clone.name = cache_key;
        clone.loc = tmpl.loc;
        clone.namespace_path = tmpl.namespace_path;
        clone.is_exported = false;
        clone.is_unsafe = tmpl.is_unsafe;
        clone.receiver_ref_qualifier = tmpl.receiver_ref_qualifier;
        clone.return_type = tmpl.return_type;
        for (const auto& [name, replacement] : type_bindings) {
            clone.return_type = substitute_type_param(clone.return_type, name, replacement);
        }
        clone.return_type = resolve_generic_type(clone.return_type, tmpl.loc);
        clone.params.reserve(tmpl.params.size());
        std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                pack_param_names[tmpl.params[i].name] = {};
                for (size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                    Param p;
                    p.name = tmpl.params[i].name + "$" + std::to_string(j);
                    p.type = concrete_pack_param_types[i][j];
                    p.require_thread_movable = tmpl.params[i].require_thread_movable;
                    p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
                    clone.params.push_back(std::move(p));
                    pack_param_names[tmpl.params[i].name].push_back(tmpl.params[i].name + "$" + std::to_string(j));
                }
                continue;
            }
            Param p;
            p.name = tmpl.params[i].name;
            p.type = tmpl.params[i].type;
            p.require_thread_movable = tmpl.params[i].require_thread_movable;
            p.require_thread_shareable = tmpl.params[i].require_thread_shareable;
            bool upcasted = false;
            for (const auto& [idx, target] : upcasts) {
                if (idx != i) continue;
                if (p.type.kind == TypeKind::Reference) {
                    p.type.pointee = std::make_shared<Type>(target);
                } else {
                    p.type = target;
                }
                upcasted = true;
                break;
            }
            if (!upcasted) {
                for (const auto& [name, replacement] : type_bindings) {
                    p.type = substitute_type_param(p.type, name, replacement);
                }
            }
            p.type = resolve_generic_type(p.type, tmpl.loc);
            clone.params.push_back(std::move(p));
        }
        clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
        if (clone.body) {
            for (const auto& [name, replacement] : type_bindings) {
                substitute_type_param_in_stmt(*clone.body, name, replacement);
            }
            for (const auto& [pack_name, concrete_names] : pack_param_names) {
                expand_pack_expansions_in_stmt(*clone.body, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
            }
            resolve_generic_types_in_stmt(*clone.body);
        }
        known_function_names_.insert(clone.name);
        program_.functions.push_back(std::move(clone));
        walk_new_concrete_function(program_.functions.size() - 1);
        return cache_key;
    }

    void monomorphize_generic_function_designator(Expr& expr, const Function& tmpl) {
        std::unordered_map<std::string, Type> type_bindings;
        std::unordered_map<std::string, int> value_bindings;
        std::unordered_map<std::string, std::vector<Type>> explicit_pack_bindings;
        std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());

        size_t explicit_index = 0;
        for (size_t p = 0; p < tmpl.template_params.size(); p++) {
            const GenericTypeParam& tp = tmpl.template_params[p];
            if (tp.is_pack) {
                std::vector<Type>& pack = explicit_pack_bindings[tp.name];
                while (explicit_index < expr.explicit_template_args.size()) {
                    const ExplicitTemplateArg& arg = expr.explicit_template_args[explicit_index++];
                    if (!arg.is_type) {
                        throw DataflowError("template parameter pack '" + tp.name + "' of generic function '" +
                                                tmpl.name + "' only accepts type arguments in this version",
                            expr.loc);
                    }
                    pack.push_back(arg.type);
                }
                continue;
            }
            if (explicit_index >= expr.explicit_template_args.size()) break;
            const ExplicitTemplateArg& arg = expr.explicit_template_args[explicit_index++];
            if (tp.is_non_type) {
                if (arg.is_type || !arg.value) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                            "' is a non-type parameter, but a type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                value_bindings[tp.name] = evaluate_non_type_arg(*arg.value, value_bindings);
            } else {
                if (!arg.is_type) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                            "' is a type parameter, but a non-type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                type_bindings[tp.name] = arg.type;
            }
        }

        if (explicit_index != expr.explicit_template_args.size()) {
            throw DataflowError("too many explicit template arguments for generic function '" + tmpl.name + "'",
                expr.loc);
        }

        for (size_t i = 0; i < tmpl.params.size(); i++) {
            if (!tmpl.params[i].is_parameter_pack) continue;
            std::optional<std::string> pack_type_name =
                referenced_type_pack_param_name(tmpl.params[i].type, tmpl.template_params);
            if (!pack_type_name.has_value()) continue;
            auto pack_it = explicit_pack_bindings.find(*pack_type_name);
            if (pack_it == explicit_pack_bindings.end()) continue;
            for (const Type& concrete : pack_it->second) {
                concrete_pack_param_types[i].push_back(substitute_type_param(tmpl.params[i].type, *pack_type_name, concrete));
            }
        }

        for (const GenericTypeParam& tp : tmpl.template_params) {
            if (tp.is_pack) continue;
            bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
            if (!bound) {
                throw DataflowError("cannot form a function designator for generic function '" + tmpl.name +
                                        "' without an explicit argument for template parameter '" + tp.name + "'",
                    expr.loc);
            }
        }

        expr.name = instantiate_full_header_generic_clone(tmpl, type_bindings, value_bindings, concrete_pack_param_types);
        expr.explicit_template_args.clear();
    }

    // ch05 §5.11: monomorphizes a call to a full-header-form generic
    // function template (Function::template_params non-empty, e.g.
    // `get`/`make`) -- binds each of the template's own parameters to a
    // concrete Type (type parameter) or int value (non-type parameter),
    // either from an explicit call-site argument (Expr::
    // explicit_template_args) or by deduction from the corresponding
    // function-parameter's own argument (an ordinary bare-`T`-shaped
    // parameter, or ch05 §5.14's own base-class-deduction accessor
    // pattern, see deduce_via_base_class_chain), then synthesizes (or
    // reuses an already-cached) concrete clone and rewrites `expr.name`
    // to it.
    void monomorphize_generic_function_call(Expr& expr, const Function& tmpl, Body& body, size_t param_offset = 0,
                                            const std::string& member_name_prefix = "") {
        std::unordered_map<std::string, Type> type_bindings;
        std::unordered_map<std::string, int> value_bindings;
        std::vector<std::pair<size_t, Type>> upcasts;
        std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());

        for (size_t p = 0; p < expr.explicit_template_args.size() && p < tmpl.template_params.size(); p++) {
            const GenericTypeParam& tp = tmpl.template_params[p];
            const ExplicitTemplateArg& arg = expr.explicit_template_args[p];
            if (tp.is_non_type) {
                if (arg.is_type || !arg.value) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                             "' is a non-type parameter, but a type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                value_bindings[tp.name] = evaluate_non_type_arg(*arg.value, value_bindings);
            } else {
                if (!arg.is_type) {
                    throw DataflowError("template parameter '" + tp.name + "' of generic function '" + tmpl.name +
                                             "' is a type parameter, but a non-type argument was given (ch05 §5.11)",
                        expr.loc);
                }
                type_bindings[tp.name] = arg.type;
            }
        }

        size_t arg_cursor = 0;
        for (size_t i = param_offset; i < tmpl.params.size() && arg_cursor < expr.args.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                std::optional<std::string> pack_type_name =
                    referenced_type_pack_param_name(tmpl.params[i].type, tmpl.template_params);
                for (; arg_cursor < expr.args.size(); arg_cursor++) {
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) continue;
                    Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    Type substituted = pack_type_name.has_value()
                                           ? substitute_type_param(tmpl.params[i].type, *pack_type_name, named)
                                           : tmpl.params[i].type;
                    concrete_pack_param_types[i].push_back(std::move(substituted));
                }
                continue;
            }
            const Type& param_type = tmpl.params[i].type;
            const Type& underlying = param_type.kind == TypeKind::Reference ? *param_type.pointee : param_type;
            if (underlying.kind != TypeKind::Named) continue;

            if (underlying.template_args.empty() && underlying.non_type_args.empty()) {
                // Case A: a bare parameter directly named after one of
                // this template's own type parameters (e.g. "T x").
                for (const GenericTypeParam& tp : tmpl.template_params) {
                    if (tp.is_non_type || tp.name != underlying.name || type_bindings.contains(tp.name)) continue;
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) continue;
                    Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    type_bindings[tp.name] = named;
                }
                arg_cursor++;
                continue;
            }
            // Case B: a base-class-deduction pattern (e.g.
            // "TupleImpl<I, Head, Tail...>& t").
            if (variadic_generic_type_names_.contains(underlying.name)) {
                deduce_via_base_class_chain(expr, arg_cursor, underlying, body, type_bindings, value_bindings, upcasts);
            }
            arg_cursor++;
        }

        for (const GenericTypeParam& tp : tmpl.template_params) {
            // ch05 §5.14: a *pack* template parameter (e.g. "Tail") can
            // only ever appear spread inside a base-class-deduction
            // pattern in this language's current scope -- never bound
            // individually (the whole pattern it's part of is replaced
            // wholesale by the deduced upcast target type instead, see
            // deduce_via_base_class_chain), so it needs no binding of
            // its own at all.
            if (tp.is_pack) continue;
            bool bound = tp.is_non_type ? value_bindings.contains(tp.name) : type_bindings.contains(tp.name);
            if (!bound) {
                throw DataflowError("cannot deduce template parameter '" + tp.name + "' of generic function '" +
                                         tmpl.name + "', and no explicit argument was given for it (ch05 §5.11)",
                    expr.loc);
            }
        }

        // ch05 §5.15: once every template parameter is bound to a
        // concrete type, check any `[[scpp::thread_movable]]`/
        // `[[scpp::thread_shareable]]`-tagged parameter's own concrete
        // (post-substitution) type actually satisfies what it requires
        // -- before synthesizing/caching a clone, so a violation is
        // reported at the call site that triggered it.
        check_thread_safety_constraints(expr, tmpl, type_bindings);

        std::string clone_name =
            instantiate_full_header_generic_clone(tmpl, type_bindings, value_bindings, concrete_pack_param_types, upcasts);
        expr.name = member_name_prefix.empty() ? clone_name : clone_name.substr(member_name_prefix.size());
        expr.explicit_template_args.clear();
    }


    // Shared by instantiate_generic_type's struct/class branches: throws
    // a precise error if `type_param` is concept-constrained and
    // `concrete_arg` doesn't structurally satisfy it -- a no-op when
    // `type_param` is bare (nothing to check).
    void check_type_param_constraint(const GenericTypeParam& type_param, const Type& concrete_arg,
                                      const std::string& template_name, SourceLocation loc) {
        if (type_param.concept_name.empty()) return;
        auto concept_it = concepts_by_name_.find(type_param.concept_name);
        if (concept_it != concepts_by_name_.end() &&
            type_satisfies_concept(concrete_arg, *concept_it->second, program_)) {
            return;
        }
        throw DataflowError("type argument '" + concrete_arg.name + "' does not satisfy concept '" +
                             type_param.concept_name + "' required by generic type '" + template_name +
                             "' (ch05 §5.14)",
            loc);
    }

    void walk_stmt(Stmt& stmt, Body& body, const std::optional<Type>& enclosing_this_type,
                   bool allow_generic_monomorphization) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) walk_expr(*stmt.init, body, enclosing_this_type, allow_generic_monomorphization);
                for (ExprPtr& arg : stmt.ctor_args) {
                    walk_expr(*arg, body, enclosing_this_type, allow_generic_monomorphization);
                }
                if (!stmt.ctor_args.empty() && stmt.type.kind == TypeKind::Named) {
                    maybe_instantiate_generic_constructor_overloads(stmt.type.name, stmt.ctor_args, body, stmt.loc);
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
            case StmtKind::Break:
            case StmtKind::Continue:
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
        if (expr.kind == ExprKind::New && expr.type.kind == TypeKind::Named) {
            maybe_instantiate_generic_constructor_overloads(expr.type.name, expr.args, body, expr.loc);
        }
        if (expr.kind == ExprKind::Call && expr.lhs == nullptr) {
            std::optional<Type> direct_call_type = infer_expr_type(expr, body, signatures_);
            bool names_known_class = false;
            if (direct_call_type.has_value() && direct_call_type->kind == TypeKind::Named) {
                for (const ClassDef& def : program_.classes) {
                    if (def.name == direct_call_type->name) {
                        names_known_class = true;
                        break;
                    }
                }
            }
            if (names_known_class) {
                maybe_instantiate_generic_constructor_overloads(direct_call_type->name, expr.args, body, expr.loc);
            }
        }

        if (expr.kind == ExprKind::Unary && expr.unary_op == UnaryOp::Deref && expr.lhs != nullptr) {
            std::optional<Type> operand_type = infer_expr_type(*expr.lhs, body, signatures_);
            if (operand_type.has_value()) {
                const Type& underlying =
                    operand_type->kind == TypeKind::Reference && operand_type->pointee ? *operand_type->pointee
                                                                                        : *operand_type;
                if (underlying.kind == TypeKind::Named &&
                    signatures_.contains(underlying.name + "_operator_deref")) {
                    ExprPtr receiver = std::move(expr.lhs);
                    expr.kind = ExprKind::Call;
                    expr.name = "operator_deref";
                    expr.lhs = std::move(receiver);
                    expr.unary_op = UnaryOp::Not;
                }
            }
        }

        if (expr.kind == ExprKind::Call && expr.lhs != nullptr && expr.name.empty()) {
            std::optional<Type> callee_type = infer_expr_type(*expr.lhs, body, signatures_);
            if (callee_type.has_value()) {
                const Type& underlying =
                    callee_type->kind == TypeKind::Reference && callee_type->pointee ? *callee_type->pointee
                                                                                      : *callee_type;
                if (underlying.kind == TypeKind::Named) expr.name = "call";
            }
        }

        // Generic-call monomorphization is suppressed entirely while
        // walking a generic template's own body (see run()'s own
        // comment): a nested generic-to-generic call is left targeting
        // the original, codegen-excluded template instead.
        if (!allow_generic_monomorphization) return;
        if (expr.kind == ExprKind::Identifier && !expr.explicit_template_args.empty()) {
            auto template_it = generic_template_indices_.find(expr.name);
            if (template_it == generic_template_indices_.end()) return;
            const Function& tmpl = program_.functions[template_it->second];
            if (tmpl.template_params.empty()) return;
            monomorphize_generic_function_designator(expr, tmpl);
            return;
        }
        if (expr.kind != ExprKind::Call) return;
        std::string generic_template_name = expr.name;
        size_t param_offset = 0;
        std::string cloned_method_suffix_prefix;
        if (expr.lhs != nullptr) {
            std::optional<Type> receiver = infer_expr_type(*expr.lhs, body, signatures_);
            if (!receiver.has_value()) return;
            const Type& receiver_named = receiver->kind == TypeKind::Reference ? *receiver->pointee : *receiver;
            if (receiver_named.kind != TypeKind::Named) return;
            generic_template_name = receiver_named.name + "_" + expr.name;
            param_offset = 1;
            cloned_method_suffix_prefix = receiver_named.name + "_";
        }
        auto template_it = generic_template_indices_.find(generic_template_name);
        if (template_it == generic_template_indices_.end()) return;
        const Function& tmpl = program_.functions[template_it->second];

        // ch05 §5.11: a full-header-form generic function template
        // (Function::template_params non-empty) is monomorphized by an
        // entirely separate mechanism (explicit-argument/base-class-
        // deduction, not this abbreviated-Concept-auto-form's own
        // per-constrained-parameter-position matching below) --
        // dispatched first and returned from immediately, since the
        // rest of this function assumes the abbreviated form's own
        // shape (Param::generic_concept) throughout.
        if (!tmpl.template_params.empty()) {
            monomorphize_generic_function_call(expr, tmpl, body, param_offset, cloned_method_suffix_prefix);
            return;
        }

        std::vector<Type> concrete_param_types;
        concrete_param_types.reserve(tmpl.params.size());
        std::vector<std::vector<Type>> concrete_pack_param_types(tmpl.params.size());
        size_t arg_cursor = 0;
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (i < param_offset) {
                concrete_param_types.push_back(param.type);
                continue;
            }
            if (param.is_parameter_pack) {
                for (; arg_cursor < expr.args.size(); arg_cursor++) {
                    std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
                    if (!arg_type.has_value()) return;
                    Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
                    if (param.generic_concept != "$auto") {
                        auto concept_it = concepts_by_name_.find(param.generic_concept);
                        if (concept_it == concepts_by_name_.end()) return;
                        if (!type_satisfies_concept(named, *concept_it->second, program_)) {
                            throw DataflowError("argument type '" + named.name + "' does not satisfy concept '" +
                                                    param.generic_concept + "' required by generic function '" +
                                                    tmpl.name +
                                                    "' (ch05 §5.11 -- every requirement's method must exist with a "
                                                    "matching signature)",
                                expr.loc);
                        }
                    }
                    Type substituted = param.type;
                    if (substituted.kind == TypeKind::Reference) {
                        substituted.pointee = std::make_shared<Type>(named);
                    } else {
                        substituted = named;
                    }
                    concrete_pack_param_types[i].push_back(std::move(substituted));
                }
                concrete_param_types.push_back(concrete_pack_param_types[i].empty() ? param.type
                                                                                    : concrete_pack_param_types[i][0]);
                continue;
            }
            if (param.generic_concept.empty()) {
                concrete_param_types.push_back(param.type);
                arg_cursor++;
                continue;
            }
            if (arg_cursor >= expr.args.size()) return; // arg-count mismatch -- leave for codegen's own error
            std::optional<Type> arg_type = infer_expr_type(*expr.args[arg_cursor], body, signatures_);
            if (!arg_type.has_value()) return;
            // The concept is checked against the argument's *underlying*
            // named type -- e.g. a `const Shape auto&` parameter's
            // argument might itself be a plain `Circle` local (arg_type
            // == Named("Circle")) or an already-bound `const Circle&`
            // reference variable (arg_type == Reference(Circle)) -- both
            // resolve to the same concrete type for concept-satisfaction
            // and substitution purposes.
            Type named = arg_type->kind == TypeKind::Reference ? *arg_type->pointee : *arg_type;
            if (param.generic_concept != "$auto") {
                auto concept_it = concepts_by_name_.find(param.generic_concept);
                if (concept_it == concepts_by_name_.end()) return;
                if (!type_satisfies_concept(named, *concept_it->second, program_)) {
                    throw DataflowError("argument type '" + named.name + "' does not satisfy concept '" +
                                           param.generic_concept + "' required by generic function '" + tmpl.name +
                                           "' (ch05 §5.11 -- every requirement's method must exist with a matching "
                                           "signature)",
                        expr.loc);
                }
            }
            Type substituted = param.type;
            if (substituted.kind == TypeKind::Reference) {
                substituted.pointee = std::make_shared<Type>(named);
            } else {
                substituted = named;
            }
            concrete_param_types.push_back(std::move(substituted));
            arg_cursor++;
        }

        // ch05 §5.15: the abbreviated (`Concept auto&`) form's own
        // identical check -- see check_thread_safety_constraints' own
        // comment; this form's own concrete param types are already
        // fully resolved in concrete_param_types by this point, so no
        // extra type_bindings substitution is needed here.
        for (size_t i = param_offset; i < tmpl.params.size(); i++) {
            const Param& param = tmpl.params[i];
            if (!param.require_thread_movable && !param.require_thread_shareable) continue;
            const std::vector<Type>* types_to_check =
                param.is_parameter_pack ? &concrete_pack_param_types[i] : nullptr;
            if (types_to_check == nullptr) {
                if (param.require_thread_movable && !is_thread_movable(concrete_param_types[i])) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                        expr.loc);
                }
                if (param.require_thread_shareable && !is_thread_shareable(concrete_param_types[i])) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                        expr.loc);
                }
                continue;
            }
            for (const Type& concrete_type : *types_to_check) {
                if (param.require_thread_movable && !is_thread_movable(concrete_type)) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_movable]]' (ch05 §5.15)",
                        expr.loc);
                }
                if (param.require_thread_shareable && !is_thread_shareable(concrete_type)) {
                    throw DataflowError("argument for parameter '" + param.name + "' of generic function '" +
                                            tmpl.name +
                                            "' does not satisfy '[[scpp::thread_shareable]]' (ch05 §5.15)",
                        expr.loc);
                }
            }
        }

        std::string clone_name = get_or_create_clone(tmpl, concrete_param_types, concrete_pack_param_types);
        if (expr.lhs == nullptr) {
            expr.name = std::move(clone_name);
        } else {
            expr.name = clone_name.substr(cloned_method_suffix_prefix.size());
        }
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
        known_type_names_.insert(class_name);

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
        call_method.is_generic_template =
            std::any_of(expr.lambda_params.begin(), expr.lambda_params.end(),
                        [](const Param& param) { return !param.generic_concept.empty(); });

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
        known_function_names_.insert(synthesized.name);
        if (synthesized.is_generic_template) {
            generic_template_indices_[synthesized.name] = program_.functions.size() - 1;
        }

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
            synthesized_body.program = &program_;
            walk_stmt(*synthesized.body, synthesized_body, this_type_of(synthesized),
                      /*allow_generic_monomorphization=*/!synthesized.is_generic_template);
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

    [[nodiscard]] bool expr_mentions_identifier(const Expr& expr, const std::string& name) const {
        if (expr.kind == ExprKind::Identifier && expr.name == name) return true;
        if (expr.lhs && expr_mentions_identifier(*expr.lhs, name)) return true;
        if (expr.rhs && expr_mentions_identifier(*expr.rhs, name)) return true;
        for (const ExprPtr& arg : expr.args) {
            if (expr_mentions_identifier(*arg, name)) return true;
        }
        if (expr.kind == ExprKind::Lambda) {
            for (const LambdaCapture& capture : expr.lambda_captures) {
                if (capture.init && expr_mentions_identifier(*capture.init, name)) return true;
            }
            if (expr.lambda_body && stmt_mentions_identifier(*expr.lambda_body, name)) return true;
        }
        return false;
    }

    [[nodiscard]] bool stmt_mentions_identifier(const Stmt& stmt, const std::string& name) const {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init && expr_mentions_identifier(*stmt.init, name)) return true;
                for (const ExprPtr& arg : stmt.ctor_args) {
                    if (expr_mentions_identifier(*arg, name)) return true;
                }
                return false;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                return stmt.expr && expr_mentions_identifier(*stmt.expr, name);
            case StmtKind::If:
                return expr_mentions_identifier(*stmt.condition, name) ||
                       stmt_mentions_identifier(*stmt.then_branch, name) ||
                       (stmt.else_branch && stmt_mentions_identifier(*stmt.else_branch, name));
            case StmtKind::While:
                return expr_mentions_identifier(*stmt.condition, name) ||
                       stmt_mentions_identifier(*stmt.then_branch, name);
            case StmtKind::Break:
            case StmtKind::Continue:
                return false;
            case StmtKind::Block:
                for (const StmtPtr& child : stmt.statements) {
                    if (stmt_mentions_identifier(*child, name)) return true;
                }
                return false;
        }
        return false;
    }

    void substitute_identifier_in_expr(Expr& expr, const std::string& from, const std::string& to) {
        if (expr.kind == ExprKind::Identifier && expr.name == from) {
            expr.name = to;
            return;
        }
        if (expr.lhs) substitute_identifier_in_expr(*expr.lhs, from, to);
        if (expr.rhs) substitute_identifier_in_expr(*expr.rhs, from, to);
        for (ExprPtr& arg : expr.args) substitute_identifier_in_expr(*arg, from, to);
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) substitute_identifier_in_expr(*capture.init, from, to);
        }
        if (expr.lambda_body) substitute_identifier_in_stmt(*expr.lambda_body, from, to);
    }

    void substitute_identifier_in_stmt(Stmt& stmt, const std::string& from, const std::string& to) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) substitute_identifier_in_expr(*stmt.init, from, to);
                for (ExprPtr& arg : stmt.ctor_args) substitute_identifier_in_expr(*arg, from, to);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) substitute_identifier_in_expr(*stmt.expr, from, to);
                return;
            case StmtKind::If:
                substitute_identifier_in_expr(*stmt.condition, from, to);
                substitute_identifier_in_stmt(*stmt.then_branch, from, to);
                if (stmt.else_branch) substitute_identifier_in_stmt(*stmt.else_branch, from, to);
                return;
            case StmtKind::While:
                substitute_identifier_in_expr(*stmt.condition, from, to);
                substitute_identifier_in_stmt(*stmt.then_branch, from, to);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) substitute_identifier_in_stmt(*s, from, to);
                return;
        }
    }

    [[nodiscard]] ExprPtr make_fold_identity(BinaryOp op, SourceLocation loc) const {
        auto node = std::make_unique<Expr>();
        node->loc = loc;
        switch (op) {
            case BinaryOp::Add:
                node->kind = ExprKind::IntegerLiteral;
                node->int_value = 0;
                return node;
            case BinaryOp::Mul:
                node->kind = ExprKind::IntegerLiteral;
                node->int_value = 1;
                return node;
            case BinaryOp::And:
                node->kind = ExprKind::BoolLiteral;
                node->bool_value = true;
                return node;
            case BinaryOp::Or:
                node->kind = ExprKind::BoolLiteral;
                node->bool_value = false;
                return node;
            default: return nullptr;
        }
    }

    [[nodiscard]] ExprPtr build_binary_expr(BinaryOp op, ExprPtr lhs, ExprPtr rhs) const {
        auto node = std::make_unique<Expr>();
        node->kind = ExprKind::Binary;
        node->binary_op = op;
        node->loc = lhs->loc;
        node->lhs = std::move(lhs);
        node->rhs = std::move(rhs);
        return node;
    }

    [[nodiscard]] ExprPtr instantiate_pack_operand(const Expr& pattern, const std::string& pack_name,
                                                   const std::string& concrete_name) {
        ExprPtr result = clone_expr(pattern);
        substitute_identifier_in_expr(*result, pack_name, concrete_name);
        return result;
    }

    [[nodiscard]] ExprPtr expand_fold_for_pack(const Expr& fold_expr, const std::string& pack_name,
                                               const std::vector<std::string>& concrete_names) {
        if (fold_expr.kind != ExprKind::Fold) return clone_expr(fold_expr);
        if (fold_expr.fold_ellipsis_on_left) {
            if (fold_expr.rhs != nullptr) {
                throw DataflowError("binary left folds are not supported in this version", fold_expr.loc);
            }
            if (concrete_names.empty()) {
                ExprPtr identity = make_fold_identity(fold_expr.binary_op, fold_expr.loc);
                if (identity) return identity;
                throw DataflowError("empty fold requires an operator identity this version does not implement",
                                    fold_expr.loc);
            }
            ExprPtr result = instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[0]);
            for (size_t i = 1; i < concrete_names.size(); i++) {
                result = build_binary_expr(fold_expr.binary_op, std::move(result),
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]));
            }
            return result;
        }

        if (fold_expr.rhs == nullptr) {
            if (concrete_names.empty()) {
                ExprPtr identity = make_fold_identity(fold_expr.binary_op, fold_expr.loc);
                if (identity) return identity;
                throw DataflowError("empty fold requires an operator identity this version does not implement",
                                    fold_expr.loc);
            }
            ExprPtr result =
                instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[concrete_names.size() - 1]);
            for (size_t i = concrete_names.size() - 1; i-- > 0;) {
                result = build_binary_expr(fold_expr.binary_op,
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]),
                                           std::move(result));
            }
            return result;
        }

        bool lhs_mentions = expr_mentions_identifier(*fold_expr.lhs, pack_name);
        bool rhs_mentions = expr_mentions_identifier(*fold_expr.rhs, pack_name);
        if (lhs_mentions == rhs_mentions) {
            throw DataflowError("fold expression must mention the parameter pack on exactly one side of '...'",
                                fold_expr.loc);
        }
        if (concrete_names.empty()) {
            return lhs_mentions ? clone_expr(*fold_expr.rhs) : clone_expr(*fold_expr.lhs);
        }
        if (lhs_mentions) {
            ExprPtr result = clone_expr(*fold_expr.rhs);
            for (size_t i = concrete_names.size(); i-- > 0;) {
                result = build_binary_expr(fold_expr.binary_op,
                                           instantiate_pack_operand(*fold_expr.lhs, pack_name, concrete_names[i]),
                                           std::move(result));
            }
            return result;
        }
        ExprPtr result = clone_expr(*fold_expr.lhs);
        for (size_t i = 0; i < concrete_names.size(); i++) {
            result = build_binary_expr(fold_expr.binary_op, std::move(result),
                                       instantiate_pack_operand(*fold_expr.rhs, pack_name, concrete_names[i]));
        }
        return result;
    }

    [[nodiscard]] std::vector<ExprPtr> expand_pack_argument(const Expr& expr, const std::string& pack_name,
                                                            const std::vector<std::string>& concrete_names) {
        if (expr.kind != ExprKind::PackExpansion || expr.lhs == nullptr) {
            std::vector<ExprPtr> single;
            single.push_back(clone_expr(expr));
            return single;
        }
        if (!expr_mentions_identifier(*expr.lhs, pack_name)) {
            throw DataflowError("pack expansion does not mention parameter pack '" + pack_name + "'", expr.loc);
        }
        std::vector<ExprPtr> expanded;
        expanded.reserve(concrete_names.size());
        for (const std::string& concrete_name : concrete_names) {
            ExprPtr arg = instantiate_pack_operand(*expr.lhs, pack_name, concrete_name);
            expanded.push_back(std::move(arg));
        }
        return expanded;
    }

    void expand_explicit_template_arg_packs_in_expr(Expr& expr, const std::string& pack_name,
                                                   const std::vector<std::string>& concrete_names) {
        if (expr.lhs) expand_explicit_template_arg_packs_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_explicit_template_arg_packs_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_explicit_template_arg_packs_in_expr(*arg, pack_name, concrete_names);
        if (!expr.explicit_template_args.empty()) {
            std::vector<ExplicitTemplateArg> expanded_template_args;
            for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
                if (arg.is_type && arg.type.is_pack_expansion && arg.type.kind == TypeKind::Named && arg.type.name == pack_name) {
                    for (const std::string& concrete_name : concrete_names) {
                        ExplicitTemplateArg expanded_arg;
                        expanded_arg.is_type = true;
                        expanded_arg.type.kind = TypeKind::Named;
                        expanded_arg.type.name = concrete_name;
                        expanded_template_args.push_back(std::move(expanded_arg));
                    }
                    continue;
                }
                expanded_template_args.push_back(std::move(arg));
            }
            expr.explicit_template_args = std::move(expanded_template_args);
        }
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_explicit_template_arg_packs_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) {
            for (StmtPtr& s : expr.lambda_body->statements) {
                switch (s->kind) {
                    case StmtKind::VarDecl:
                        if (s->init) expand_explicit_template_arg_packs_in_expr(*s->init, pack_name, concrete_names);
                        for (ExprPtr& a : s->ctor_args) expand_explicit_template_arg_packs_in_expr(*a, pack_name, concrete_names);
                        break;
                    case StmtKind::Return:
                    case StmtKind::ExprStmt:
                        if (s->expr) expand_explicit_template_arg_packs_in_expr(*s->expr, pack_name, concrete_names);
                        break;
                    case StmtKind::If:
                    case StmtKind::While:
                        expand_explicit_template_arg_packs_in_expr(*s->condition, pack_name, concrete_names);
                        break;
                    case StmtKind::Break:
                    case StmtKind::Continue:
                        break;
                    case StmtKind::Block:
                        break;
                }
            }
        }
    }

    void expand_pack_expansions_in_expr(Expr& expr, const std::string& pack_name,
                                        const std::vector<std::string>& concrete_names) {
        if (expr.lhs) expand_pack_expansions_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_pack_expansions_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_pack_expansions_in_expr(*arg, pack_name, concrete_names);
        if (!expr.explicit_template_args.empty()) {
            std::vector<ExplicitTemplateArg> expanded_template_args;
            for (ExplicitTemplateArg& arg : expr.explicit_template_args) {
                if (arg.is_type && arg.type.is_pack_expansion && arg.type.kind == TypeKind::Named && arg.type.name == pack_name) {
                    for (const std::string& concrete_name : concrete_names) {
                        ExplicitTemplateArg expanded_arg;
                        expanded_arg.is_type = true;
                        expanded_arg.type.kind = TypeKind::Named;
                        expanded_arg.type.name = concrete_name;
                        expanded_template_args.push_back(std::move(expanded_arg));
                    }
                    continue;
                }
                expanded_template_args.push_back(std::move(arg));
            }
            expr.explicit_template_args = std::move(expanded_template_args);
        }
        if (!expr.args.empty()) {
            std::vector<ExprPtr> expanded_args;
            for (ExprPtr& arg : expr.args) {
                std::vector<ExprPtr> expanded = expand_pack_argument(*arg, pack_name, concrete_names);
                for (ExprPtr& item : expanded) expanded_args.push_back(std::move(item));
            }
            expr.args = std::move(expanded_args);
        }
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_pack_expansions_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) expand_pack_expansions_in_stmt(*expr.lambda_body, pack_name, concrete_names);
    }

    void expand_explicit_template_arg_packs_in_stmt(Stmt& stmt, const std::string& pack_name,
                                                   const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_explicit_template_arg_packs_in_expr(*stmt.init, pack_name, concrete_names);
                for (ExprPtr& arg : stmt.ctor_args) expand_explicit_template_arg_packs_in_expr(*arg, pack_name, concrete_names);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_explicit_template_arg_packs_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_explicit_template_arg_packs_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_explicit_template_arg_packs_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_explicit_template_arg_packs_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_explicit_template_arg_packs_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_explicit_template_arg_packs_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_explicit_template_arg_packs_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    void expand_pack_folds_in_expr(Expr& expr, const std::string& pack_name, const std::vector<std::string>& concrete_names) {
        if (expr.kind == ExprKind::Fold &&
            (expr_mentions_identifier(expr, pack_name) ||
             (!expr.rhs && expr_mentions_identifier(*expr.lhs, pack_name)))) {
            ExprPtr expanded = expand_fold_for_pack(expr, pack_name, concrete_names);
            expr = std::move(*expanded);
        }
        if (expr.lhs) expand_pack_folds_in_expr(*expr.lhs, pack_name, concrete_names);
        if (expr.rhs) expand_pack_folds_in_expr(*expr.rhs, pack_name, concrete_names);
        for (ExprPtr& arg : expr.args) expand_pack_folds_in_expr(*arg, pack_name, concrete_names);
        for (LambdaCapture& capture : expr.lambda_captures) {
            if (capture.init) expand_pack_folds_in_expr(*capture.init, pack_name, concrete_names);
        }
        if (expr.lambda_body) expand_pack_folds_in_stmt(*expr.lambda_body, pack_name, concrete_names);
    }

    void expand_pack_folds_in_stmt(Stmt& stmt, const std::string& pack_name, const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_pack_folds_in_expr(*stmt.init, pack_name, concrete_names);
                for (ExprPtr& arg : stmt.ctor_args) expand_pack_folds_in_expr(*arg, pack_name, concrete_names);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_pack_folds_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_pack_folds_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_pack_folds_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_pack_folds_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_folds_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_pack_folds_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    void expand_pack_expansions_in_stmt(Stmt& stmt, const std::string& pack_name,
                                        const std::vector<std::string>& concrete_names) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                if (stmt.init) expand_pack_expansions_in_expr(*stmt.init, pack_name, concrete_names);
                if (!stmt.ctor_args.empty()) {
                    for (ExprPtr& arg : stmt.ctor_args) expand_pack_expansions_in_expr(*arg, pack_name, concrete_names);
                    std::vector<ExprPtr> expanded_args;
                    for (ExprPtr& arg : stmt.ctor_args) {
                        std::vector<ExprPtr> expanded = expand_pack_argument(*arg, pack_name, concrete_names);
                        for (ExprPtr& item : expanded) expanded_args.push_back(std::move(item));
                    }
                    stmt.ctor_args = std::move(expanded_args);
                }
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) expand_pack_expansions_in_expr(*stmt.expr, pack_name, concrete_names);
                return;
            case StmtKind::If:
                expand_pack_expansions_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_expansions_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                if (stmt.else_branch) expand_pack_expansions_in_stmt(*stmt.else_branch, pack_name, concrete_names);
                return;
            case StmtKind::While:
                expand_pack_expansions_in_expr(*stmt.condition, pack_name, concrete_names);
                expand_pack_expansions_in_stmt(*stmt.then_branch, pack_name, concrete_names);
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
            case StmtKind::Block:
                for (StmtPtr& s : stmt.statements) expand_pack_expansions_in_stmt(*s, pack_name, concrete_names);
                return;
        }
    }

    std::string get_or_create_clone(const Function& tmpl, const std::vector<Type>& concrete_param_types,
                                    const std::vector<std::vector<Type>>& concrete_pack_param_types) {
        std::string cache_key = tmpl.name;
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                for (const Type& t : concrete_pack_param_types[i]) cache_key += "." + mangle_type_for_clone_name(t);
            } else {
                cache_key += "." + mangle_type_for_clone_name(concrete_param_types[i]);
            }
        }
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
        clone.is_unsafe = tmpl.is_unsafe;
        std::unordered_map<std::string, Type> witness_replacements;
        for (size_t i = 0; i < tmpl.params.size() && i < concrete_param_types.size(); i++) {
            if (!tmpl.params[i].generic_concept.empty()) {
                const Type& concrete = concrete_param_types[i].kind == TypeKind::Reference
                                           ? *concrete_param_types[i].pointee
                                           : concrete_param_types[i];
                witness_replacements[tmpl.params[i].generic_concept] = concrete;
            }
        }
        for (const auto& [witness_name, concrete] : witness_replacements) {
            clone.return_type = substitute_type_param(clone.return_type, witness_name, concrete);
        }
        clone.params.reserve(tmpl.params.size());
        std::unordered_map<std::string, std::vector<std::string>> pack_param_names;
        for (size_t i = 0; i < tmpl.params.size(); i++) {
            if (tmpl.params[i].is_parameter_pack) {
                pack_param_names[tmpl.params[i].name] = {};
                for (size_t j = 0; j < concrete_pack_param_types[i].size(); j++) {
                    Param p;
                    p.name = tmpl.params[i].name + "$" + std::to_string(j);
                    p.type = concrete_pack_param_types[i][j];
                    clone.params.push_back(p);
                    pack_param_names[tmpl.params[i].name].push_back(p.name);
                }
                continue;
            }
            Param p;
            p.name = tmpl.params[i].name;
            p.type = concrete_param_types[i];
            clone.params.push_back(std::move(p));
        }
        clone.body = tmpl.body ? clone_stmt(*tmpl.body) : nullptr;
        if (clone.body) {
            for (const auto& [witness_name, concrete] : witness_replacements) {
                substitute_type_param_in_stmt(*clone.body, witness_name, concrete);
            }
            for (const auto& [pack_name, concrete_names] : pack_param_names) {
                expand_pack_folds_in_stmt(*clone.body, pack_name, concrete_names);
            }
        }
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
    // See DataflowState::class_field_access's own comment -- struct
    // fields have no access control at all (ch04 §4.1), so only
    // program.classes populates this.
    ClassFieldAccess class_field_access;
    for (const ClassDef& def : program.classes) {
        for (const ClassField& field : def.fields) {
            class_field_access[def.name][field.name] = field.access;
        }
    }
    // spec §6.5: every class eligible for copy construction/assignment
    // (user-declared or compiler-provided) -- see DataflowState's own
    // comment and is_copy_constructible/is_copy_assignable. No cycle
    // protection needed unlike ch05 §5.15's thread-safety derivation: a
    // class can never contain itself by value (infinite size), so the
    // field-containment recursion this walks is always a DAG, not a
    // graph that could cycle.
    std::unordered_set<std::string> classes_with_copy_ctor;
    std::unordered_set<std::string> classes_with_copy_assign;
    for (const ClassDef& def : program.classes) {
        if (is_copy_constructible(def.name, program)) classes_with_copy_ctor.insert(def.name);
        if (is_copy_assignable(def.name, program)) classes_with_copy_assign.insert(def.name);
    }
    // ch05 §5.11: every concept/bare-`auto` witness class (never a real,
    // user-written one) -- see ClassDef::is_concept_witness and
    // check_function's own by-value-parameter/return-type exemption for
    // why this is needed.
    std::unordered_set<std::string> witness_class_names;
    for (const ClassDef& def : program.classes) {
        if (def.is_concept_witness) witness_class_names.insert(def.name);
    }
    for (const Function& fn : program.functions) {
        // A bodyless `extern "C"` declaration (ch02 §2.1) has no
        // statements to run the dataflow analysis over -- it's already
        // registered in `signatures` above (so call sites into it are
        // still checked normally), but there's nothing here to check.
        if (!fn.body) continue;
        // ch05 §5.11: a full-header-form generic function's own
        // template (e.g. `get`/`make`, Function::template_params
        // non-empty) is never checked directly here -- its own body may
        // reference a not-yet-bound template parameter's own name as if
        // it were a real type (e.g. `T x;`, or a base-class-deduction
        // pattern's own "Head"/"Tail"), which movecheck's Body-based
        // machinery has no way to make sense of abstractly (unlike the
        // abbreviated `Concept auto` form, whose constrained parameter's
        // declared type already names a real, though synthetic, witness
        // class). Only each concrete call site's own monomorphized
        // clone (an ordinary, fully-concrete Function by the time this
        // runs, synthesized by monomorphize_generic_function_call) is
        // ever checked -- a narrower, pragmatic scope than the
        // abbreviated form's "checked once abstractly, independent of
        // any call site" guarantee, accepted given this form's added
        // deduction-pattern complexity.
        if (!fn.template_params.empty()) continue;
        if (!fn.generic_method_owner_id.empty()) continue;
        check_function(fn, program, signatures, class_names, class_field_types, class_field_access, classes_with_copy_ctor,
                       classes_with_copy_assign, witness_class_names);
    }
}

} // namespace scpp
