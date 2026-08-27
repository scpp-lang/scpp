module;

export module scpp.constexpression;

import std;
import scpp.ast;

export namespace scpp {

struct ConstexprLimits {
    int max_steps = 1000000;
    // A *language* limit: how deep a program's constant evaluation may
    // recurse. It must therefore be host- and build-independent, which is
    // exactly what the three previous values were not. 512 and its
    // replacement 256 were unreachable in the project's default (and CI's)
    // Debug build -- the host stack died first, so the diagnostic below
    // could never be produced and the compiler segfaulted instead. 128 was
    // reachable, but ch06 (6.4) requires an implementation to support "no
    // less than 512 nested evaluations", so 128 bought crash-freedom at
    // the price of conformance.
    //
    // 512 is now both conforming and reachable, because the frames it is
    // multiplied by were made small enough to fit. The engine's
    // mutually-recursive walk (call_function -> execute_stmt ->
    // evaluate_expr -> evaluate_call_expr -> call_function) cost a
    // perfectly linear 36,802 bytes of host stack per level in a Debug
    // build; extracting this file's switch arms and branch bodies into
    // immediately-invoked lambdas brought that to 10,714 bytes per level,
    // measured the same way (bisecting the minimum `ulimit -s` a given
    // depth survives: 671 KiB at depth 60, 1,090 KiB at 100, 1,372 KiB at
    // 127). 512 levels therefore consume 5.27 MiB, which fits inside
    // max_stack_bytes below, which in turn sits below the 8 MiB stack that
    // is the default nearly everywhere. Because that keeps *this* counter
    // the binding limit in every build, a program is accepted or rejected
    // at the same depth whether the compiler was built Debug or Release --
    // an optimized build makes the frames far smaller, but must not
    // thereby accept programs a Debug build rejects.
    //
    // The reason the frames were enormous, and the reason a constant kept
    // being lowered instead: at -O0 clang gives every local its own stack
    // slot and runs no stack colouring, so a switch dispatcher's frame is
    // the *sum* of all its arms even though only one can be live. Brace
    // scoping does not help -- these arms were already braced. Only moving
    // an arm's body into a callee makes the peak `dispatcher + one arm`.
    int max_recursion_depth = 512;
    // An *implementation* safety property, and a different question from
    // the one above: how much host stack this engine may consume before it
    // must stop rather than die. No compile-time depth constant can answer
    // it, because the bytes-per-level it would have to be divided by are
    // not fixed -- they change with the build type (36,792 in Debug,
    // far smaller optimized), and with any future edit to the evaluation
    // walk's frames. Measuring the bytes directly is what makes "this
    // limit reports, it does not crash" a guarantee instead of a property
    // that happens to hold until someone adds a local variable.
    //
    // 6 MiB sits above the 4.49 MiB that max_recursion_depth can consume
    // (so the depth limit stays the binding one, per above) and ~1.9 MiB
    // below the 8 MiB stack where the crash was measured, which is some 50
    // further Debug frames of headroom for unwinding the diagnostic back
    // out. It assumes an 8 MiB host stack; reading the real rlimit would
    // need a POSIX header, and this module must stay compilable by scpp
    // itself, which rejects a global module fragment outright.
    std::size_t max_stack_bytes = static_cast<std::size_t>(6 * 1024 * 1024);
    int max_loop_iterations = 262144;
};

// A base-class mem-initializer cannot bind a temporary, so the "line:column: "
// prefix cannot be built inline; scpp does accept the by-value result of a
// free function there, which is what this exists for. (A static member
// function does *not* work in that position.) The body uses `+=` rather than
// chained `+` because scpp has no `operator+` on strings.
[[nodiscard]] std::string format_constexpr_error_message(const SourceLocation& loc, const std::string& message) {
    std::string formatted{};
    formatted += std::to_string(static_cast<std::int64_t>(loc.line));
    formatted += ":";
    formatted += std::to_string(static_cast<std::int64_t>(loc.column));
    formatted += ": ";
    formatted += message;
    return formatted;
}

// `class`, not `struct`: scpp only lets a `struct` hold scalars, pointers,
// trivial structs/unions and fixed-size arrays of those (spec ch04), and
// `struct X : Base` is not spellable at all -- a base clause requires
// `class` with an explicit access specifier. Same shape as parser.cppm's
// ParseError, which derives from std::runtime_error for the same reason.
class ConstexprError : public std::runtime_error {
public:
    ConstexprError(const SourceLocation& loc, const std::string& message)
        : runtime_error{format_constexpr_error_message(loc, message)}, loc{loc} {}

    // Explicit copy constructor, mirroring ParseError's: scpp's
    // std::runtime_error (std_stdexcept.scpp) declares no copy constructor
    // of its own, so the base has to be rebuilt from what() rather than
    // copy-initialized. Needed so a ConstexprError can travel into a
    // std::expected<T, ConstexprError> through std::expected's copy-based
    // std::unexpected<E> converting constructor.
    ConstexprError(const ConstexprError& other) : runtime_error{std::string{other.what()}}, loc{other.loc} {}

    virtual ~ConstexprError() override = default;

    SourceLocation loc{};
};

enum class ConstexprValueKind {
    Void,
    Integer,
    Double,
    Bool,
    StringLiteralPointer,
    Object,
    Array,
};

class ConstexprValue;

// One entry of ConstexprValue::object_fields, replacing the
// std::pair<std::string, std::shared_ptr<ConstexprValue>> it used to be:
// scpp has no <utility>/std::pair. The members keep pair's `first`/`second`
// names on purpose so the sole consumer -- codegen/expressions.cppm's
// find_if over object_fields -- compiles against this unchanged.
class ConstexprField {
public:
    virtual ~ConstexprField() = default;
    ConstexprField() = default;
    ConstexprField(const ConstexprField&) = default;
    ConstexprField& operator=(const ConstexprField&) = default;
    ConstexprField(std::string name, std::shared_ptr<ConstexprValue> value)
        : first{std::move(name)}, second{std::move(value)} {}

    std::string first{};
    std::shared_ptr<ConstexprValue> second{};
};

class ConstexprValue {
public:
    virtual ~ConstexprValue() = default;
    ConstexprValue() = default;
    ConstexprValue(const ConstexprValue&) = default;
    ConstexprValue& operator=(const ConstexprValue&) = default;

    Type type{};
    ConstexprValueKind kind = ConstexprValueKind::Void;
    std::int64_t int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    std::string string_value{};
    std::vector<ConstexprField> object_fields{};
    std::vector<ConstexprValue> elements{};
};

[[nodiscard]] std::expected<void, ConstexprError> fold_immediate_calls(Program& program, ConstexprLimits limits = {});
[[nodiscard]] std::expected<ConstexprValue, ConstexprError> evaluate_immediate_expr(const Program& program, const Expr& expr,
                                                     ConstexprLimits limits = {});

} // namespace scpp

namespace scpp {

class Cell;

// Replaces std::numeric_limits, which scpp has no <limits> for. Written as
// `-max - 1` rather than as a negative literal because the magnitude of the
// most negative value is not representable as a positive literal of its own
// type. Not `inline constexpr`: scpp has no `inline` variables (it parses
// `inline` as a function specifier), and in a module interface these are
// one entity across all importers anyway, so `inline` bought nothing here.
constexpr std::int64_t int32_max_value = 2147483647;
constexpr std::int64_t int32_min_value = -int32_max_value - 1;
constexpr std::int64_t uint32_max_value = 4294967295;
constexpr std::int64_t int64_max_value = 9223372036854775807;
constexpr std::int64_t int64_min_value = -int64_max_value - 1;

// Inclusive value range of an integer type. Replaces the
// std::pair<std::int64_t, std::int64_t> integer_bounds_for_type returned;
// scpp has no <utility>/std::pair, and no structured bindings to unpack one.
class IntegerBounds {
public:
    virtual ~IntegerBounds() = default;
    IntegerBounds() = default;
    IntegerBounds(const IntegerBounds&) = default;
    IntegerBounds& operator=(const IntegerBounds&) = default;
    IntegerBounds(std::int64_t min, std::int64_t max) : min_value{min}, max_value{max} {}

    std::int64_t min_value = 0;
    std::int64_t max_value = 0;
};

[[nodiscard]] std::expected<void, ConstexprError> rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value);
[[nodiscard]] std::expected<ConstexprValue, ConstexprError> snapshot_constexpr_value(const std::shared_ptr<Cell>& value, const SourceLocation& loc);

class PointerValue {
public:
    virtual ~PointerValue() = default;
    PointerValue() = default;
    PointerValue(const PointerValue&) = default;
    PointerValue& operator=(const PointerValue&) = default;

    std::shared_ptr<Cell> storage{};
    std::string storage_id{};
    std::int64_t index = 0;
};

class SpanValue {
public:
    virtual ~SpanValue() = default;
    SpanValue() = default;
    SpanValue(const SpanValue&) = default;
    SpanValue& operator=(const SpanValue&) = default;

    PointerValue pointer{};
    std::int64_t size = 0;
};

// One field of an ObjectValue. ObjectValue::fields used to be an
// unordered_map<std::string, std::shared_ptr<Cell>>, but scpp's
// std::unordered_map has no begin()/iteration at all, and three sites here
// iterate it. A vector also makes field order *insertion order* -- i.e. the
// declaration order the fields were created in -- rather than hash order;
// see snapshot_constexpr_value for why that matters.
class ObjectField {
public:
    virtual ~ObjectField() = default;
    ObjectField() = default;
    ObjectField(const ObjectField&) = default;
    ObjectField& operator=(const ObjectField&) = default;
    ObjectField(std::string field_name, std::shared_ptr<Cell> field_cell)
        : name{std::move(field_name)}, cell{std::move(field_cell)} {}

    std::string name{};
    std::shared_ptr<Cell> cell{};
};

class ObjectValue {
public:
    virtual ~ObjectValue() = default;
    ObjectValue() = default;
    ObjectValue(const ObjectValue&) = default;
    ObjectValue& operator=(const ObjectValue&) = default;

    std::string type_name{};
    std::vector<ObjectField> fields{};

    // Index of the field named `name`, or -1 if there is none. Replaces the
    // `find(name) != end()` idiom the unordered_map version used; objects
    // here have a handful of fields, so the linear scan is not a concern.
    [[nodiscard]] std::int64_t field_index(const std::string& name) const {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].name == name) return static_cast<std::int64_t>(i);
        }
        return -1;
    }

    // First insertion of a name wins, exactly as unordered_map::emplace did.
    // This is load-bearing, not defensive: collect_class_fields flattens base
    // and derived fields into one list without de-duplicating, so a derived
    // class that redeclares a base field's name yields that name twice.
    // std::format's validators do exactly this --
    // `format_string<Head, Tail...> : private format_string<Tail...>` and both
    // levels declare `const char* text_{}` -- so a 6-argument std::println
    // produces seven `text_` entries. The map silently collapsed them; a plain
    // emplace_back would keep all seven, and only the one the constructor
    // reaches gets initialized, leaving the rest as null const char* that
    // snapshot_constexpr_value rejects with "unsupported constexpr pointer
    // result".
    void add_field(std::string name, std::shared_ptr<Cell> cell) {
        if (field_index(name) >= 0) return;
        ObjectField field{std::move(name), std::move(cell)};
        fields.push_back(std::move(field));
    }
};

class ArrayValue {
public:
    virtual ~ArrayValue() = default;
    ArrayValue() = default;
    ArrayValue(const ArrayValue&) = default;
    ArrayValue& operator=(const ArrayValue&) = default;

    Type element_type{};
    std::vector<std::shared_ptr<Cell>> elements{};
};

// What a Cell currently holds. This is a hand-rolled tagged union rather
// than a std::variant because scpp has neither std::variant nor the
// machinery a variant needs: no `if constexpr`, no `std::is_same_v` /
// `std::decay_t`, and no way to write std::visit's generic-lambda dispatch.
// A real union is no help either -- scpp unions hold trivial types only,
// and four of these alternatives own heap memory.
//
// The shape follows ConstexprValue above (a `kind` plus one field per
// alternative), which already does exactly this in this file's public API.
// The cost is that a Cell is now the *sum* of its alternatives rather than
// the max, since the four class-type payloads can no longer overlap.
enum class CellKind {
    Empty,
    Integer,
    Double,
    Bool,
    Pointer,
    Span,
    Object,
    Array,
};

// Read a payload only after checking `kind` (or calling the matching
// is_*() predicate) -- exactly the discipline std::get<T> used to enforce
// by aborting. Write only through the set_*() methods, so `kind` can never
// disagree with the payload it describes.
class CellData {
public:
    virtual ~CellData() = default;
    CellData() = default;
    CellData(const CellData&) = default;
    CellData& operator=(const CellData&) = default;

    CellKind kind = CellKind::Empty;
    std::int64_t int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    PointerValue pointer{};
    SpanValue span{};
    ObjectValue object{};
    ArrayValue array{};

    [[nodiscard]] bool is_empty() const { return kind == CellKind::Empty; }
    [[nodiscard]] bool is_integer() const { return kind == CellKind::Integer; }
    [[nodiscard]] bool is_double() const { return kind == CellKind::Double; }
    [[nodiscard]] bool is_bool() const { return kind == CellKind::Bool; }
    [[nodiscard]] bool is_pointer() const { return kind == CellKind::Pointer; }
    [[nodiscard]] bool is_span() const { return kind == CellKind::Span; }
    [[nodiscard]] bool is_object() const { return kind == CellKind::Object; }
    [[nodiscard]] bool is_array() const { return kind == CellKind::Array; }

    void set_empty() { become(CellKind::Empty); }
    void set_integer(std::int64_t value) {
        become(CellKind::Integer);
        int_value = value;
    }
    void set_double(double value) {
        become(CellKind::Double);
        double_value = value;
    }
    void set_bool(bool value) {
        become(CellKind::Bool);
        bool_value = value;
    }
    void set_pointer(PointerValue value) {
        become(CellKind::Pointer);
        pointer = std::move(value);
    }
    void set_span(SpanValue value) {
        become(CellKind::Span);
        span = std::move(value);
    }
    void set_object(ObjectValue value) {
        become(CellKind::Object);
        object = std::move(value);
    }
    void set_array(ArrayValue value) {
        become(CellKind::Array);
        array = std::move(value);
    }

private:
    // Release whatever the cell held before, so switching kinds drops the
    // old payload's storage (and, for Object/Array, its shared_ptr<Cell>
    // references) the way destroying a std::variant alternative did.
    // Scalars need no clearing, and a same-kind write overwrites its own
    // payload anyway.
    void become(CellKind new_kind) {
        if (kind != new_kind) {
            switch (kind) {
                case CellKind::Pointer: pointer = PointerValue{}; break;
                case CellKind::Span: span = SpanValue{}; break;
                case CellKind::Object: object = ObjectValue{}; break;
                case CellKind::Array: array = ArrayValue{}; break;
                case CellKind::Empty:
                case CellKind::Integer:
                case CellKind::Double:
                case CellKind::Bool: break;
            }
        }
        kind = new_kind;
    }
};

class Cell {
public:
    virtual ~Cell() = default;
    Cell() = default;
    Cell(const Cell&) = default;
    Cell& operator=(const Cell&) = default;

    Type type{};
    CellData data{};
};

// Binding, LValue, ExprRewrite and ExecOutcome each get an explicit
// constructor matching what used to be aggregate initialization: a class
// with a declared destructor is not an aggregate, and scpp does not do
// positional brace-aggregate init in the first place. Declaring the
// constructors keeps every existing `Binding{cell, false}`-style call site
// working unchanged.
class Binding {
public:
    virtual ~Binding() = default;
    Binding() = default;
    Binding(std::shared_ptr<Cell> cell, bool read_only) : cell{std::move(cell)}, read_only{read_only} {}
    Binding(const Binding&) = default;
    Binding& operator=(const Binding&) = default;

    std::shared_ptr<Cell> cell{};
    bool read_only = false;
};

class LValue {
public:
    virtual ~LValue() = default;
    LValue() = default;
    LValue(std::shared_ptr<Cell> cell, bool read_only) : cell{std::move(cell)}, read_only{read_only} {}
    LValue(const LValue&) = default;
    LValue& operator=(const LValue&) = default;

    std::shared_ptr<Cell> cell{};
    bool read_only = false;
};

// scpp has no nullable-reference type and rejects dereferencing a raw
// pointer outside `[[scpp::unsafe]] { }` (ch01 §1.3/ch02), so every "found
// it, or didn't" lookup below returns an optional reference instead of a
// raw `T*`. This is the same shape ast.cppm's own find_struct/find_class/
// find_enum already use, and it keeps the found-object accesses inside the
// safety checker rather than escaping it.
using OptionalExprRef = std::optional<std::reference_wrapper<const Expr>>;
using OptionalInitializerRef = std::optional<std::reference_wrapper<const Initializer>>;
using OptionalFunctionRef = std::optional<std::reference_wrapper<const Function>>;
using OptionalStructDefRef = std::optional<std::reference_wrapper<StructDef>>;
using OptionalClassDefRef = std::optional<std::reference_wrapper<ClassDef>>;

// scpp cannot yet construct through an alias of a template specialization
// (`OptionalFunctionRef{...}` is rejected with "cannot deduce template
// arguments"), so every non-empty optional reference is built by one of
// these helpers, which spell the specialization out in full. The empty
// case is a bare `return {}`.
[[nodiscard]] OptionalExprRef make_expr_ref(const Expr& expr) {
    return std::optional<std::reference_wrapper<const Expr>>{std::reference_wrapper<const Expr>{expr}};
}

[[nodiscard]] OptionalInitializerRef make_initializer_ref(const Initializer& initializer) {
    return std::optional<std::reference_wrapper<const Initializer>>{std::reference_wrapper<const Initializer>{initializer}};
}

[[nodiscard]] OptionalFunctionRef make_function_ref(const Function& fn) {
    return std::optional<std::reference_wrapper<const Function>>{std::reference_wrapper<const Function>{fn}};
}

[[nodiscard]] OptionalStructDefRef make_struct_def_ref(StructDef& def) {
    return std::optional<std::reference_wrapper<StructDef>>{std::reference_wrapper<StructDef>{def}};
}

[[nodiscard]] OptionalClassDefRef make_class_def_ref(ClassDef& def) {
    return std::optional<std::reference_wrapper<ClassDef>>{std::reference_wrapper<ClassDef>{def}};
}

// A possibly-empty ExprPtr, as an optional reference. `sp.get()` would
// hand back a raw pointer the safety checker then refuses to dereference.
[[nodiscard]] OptionalExprRef optional_expr_ref(const ExprPtr& expr) {
    if (!expr) return {};
    return make_expr_ref(*expr);
}

class ExprRewrite {
public:
    virtual ~ExprRewrite() = default;
    ExprRewrite(Expr& target, std::shared_ptr<Cell> value)
        : target{std::reference_wrapper<Expr>{target}}, value{std::move(value)} {}
    ExprRewrite(const ExprRewrite&) = default;
    ExprRewrite& operator=(const ExprRewrite&) = default;

    // Never empty: every ExprRewrite is built from a live Expr during the
    // traversal below, so this is a reference rather than a raw pointer.
    std::reference_wrapper<Expr> target;
    std::shared_ptr<Cell> value{};
};

// scpp (the language) has no exceptions, so `execute_stmt`'s own
// `return`/`break`/`continue` control flow -- previously modeled by
// throwing/catching ReturnSignal/BreakSignal/ContinueSignal -- is instead
// folded into the success channel of `execute_stmt`'s own
// std::expected<ExecOutcome, ConstexprError> return type: `flow` says
// which (if any) of the three unwound out of the executed statement, and
// `return_value` carries a Return's own value (null for a `return;` with
// no operand, or for any other flow). Every caller that recurses into a
// nested statement must check `flow` itself and decide whether to consume
// it (a loop consuming its own Break/Continue) or propagate it unchanged
// to its own caller (exactly mirroring which exception types the old
// per-construct `catch` clauses used to leave uncaught).
enum class ExecFlow { Normal, Return, Break, Continue };

class ExecOutcome {
public:
    virtual ~ExecOutcome() = default;
    ExecOutcome() = default;
    ExecOutcome(ExecFlow flow, std::shared_ptr<Cell> return_value) : flow{flow}, return_value{std::move(return_value)} {}
    ExecOutcome(const ExecOutcome&) = default;
    ExecOutcome& operator=(const ExecOutcome&) = default;

    ExecFlow flow = ExecFlow::Normal;
    std::shared_ptr<Cell> return_value{};
};

// Spells out a deliberately discarded [[nodiscard]] result. The three call
// sites below are the ones the pre-std::expected code wrote as an explicit
// `try { ... } catch (const ConstexprError&) {}`; each has a comment saying
// why its error cannot matter. They used to be written `(void)call()`, which
// scpp rejects: it allows casts only between builtin scalar types (and `void`
// is not one), so neither `(void)x` nor `static_cast<void>(x)` is available.
//
// The shape is forced from both sides. The parameter is by value because
// scpp's borrow rule cannot bind a reference to a call's temporary result,
// and it is named and read because scpp has neither unnamed parameters nor
// [[maybe_unused]] while C++ has -Wextra's -Wunused-parameter. Returning
// whether the call succeeded satisfies both; the result is intentionally not
// [[nodiscard]], since dropping it is the entire point.
bool ignore_result(std::expected<ExecOutcome, ConstexprError> result) { return result.has_value(); }

bool ignore_result(std::expected<void, ConstexprError> result) { return result.has_value(); }

// Type identity comes from scpp::types_equal (scpp.ast), the one module
// this one is allowed to see besides std. This file used to carry its own
// copy, which compared non_type_args by *count* -- so `Buf<4>` and
// `Buf<8>` were distinguishable from `Buf<4, 4>` but not from each other.

[[nodiscard]] Type make_const_char_pointer_type() {
    Type result{};
    result.kind = TypeKind::Pointer;
    result.pointee = std::make_shared<Type>(named_type("char"));
    result.is_mutable_pointee = false;
    return result;
}

// The `name` parameters below are `const char*`, not std::string_view:
// scpp's overload resolution is exact-type-match only and its std::string
// has `operator==(const char*)` but no string_view overload, and every
// caller of these three passes a string literal, so `const char*` is both
// what the callers already have and what the comparison needs.
[[nodiscard]] bool is_named_type(const Type& type, const char* name) {
    return type.kind == TypeKind::Named && type.name == name && type.template_args.empty();
}

// Conversely, every caller of the lookups below already holds a
// std::string (a Type::name, an Expr::name, or a locally built name), so
// taking one by reference removes both the view conversion and the
// std::string round-trip several of these bodies used to do.
// Deliberately wider than `scpp::is_integral_scalar_type_name`: this
// asks "can constant evaluation read this as an integer", and a `bool`
// cell stores its value in the same `int_value` field an integer does.
// That is a different question from "is this an integral scalar type",
// which is why it gets its own name -- but it is still derived, so it
// cannot drift from the twenty-name set in `scalar_type_info`.
[[nodiscard]] bool is_integral_named_type(const std::string& name) {
    std::string_view view{name};
    return scpp::is_integral_scalar_type_name(view) || view == "bool";
}

[[nodiscard]] bool is_integer_like(const Type& type) {
    return type.kind == TypeKind::Named && is_integral_named_type(type.name);
}

[[nodiscard]] bool is_floating_like(const Type& type) {
    return type.kind == TypeKind::Named && scpp::is_float_scalar_type_name(std::string_view{type.name});
}

[[nodiscard]] Type make_pointer_type_to(const Type& pointee, bool is_mutable_pointee) {
    Type type{};
    type.kind = TypeKind::Pointer;
    type.pointee = std::make_shared<Type>(pointee);
    type.is_mutable_pointee = is_mutable_pointee;
    return type;
}

[[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> dereference_pointer(const PointerValue& pointer, const Type& pointer_type,
                                                        const SourceLocation& loc) {
    if (!pointer.storage) return std::unexpected(ConstexprError(loc, "constexpr dereference requires a non-null pointer"));
    if (pointer_type.kind != TypeKind::Pointer || !pointer_type.pointee) {
        return std::unexpected(ConstexprError(loc, "malformed constexpr pointer type"));
    }
    if (types_equal(*pointer_type.pointee, pointer.storage->type)) {
        if (pointer.index != 0) {
            return std::unexpected(ConstexprError(loc, "constexpr pointer arithmetic escaped the pointed-to object"));
        }
        return pointer.storage;
    }
    if (!pointer.storage->data.is_array()) {
        return std::unexpected(ConstexprError(loc, "constexpr pointer does not point to supported storage"));
    }
    const ArrayValue& array = pointer.storage->data.array;
    if (!types_equal(*pointer_type.pointee, array.element_type)) {
        return std::unexpected(ConstexprError(loc, "constexpr pointer element type does not match the pointed-to storage"));
    }
    if (pointer.index < 0 || static_cast<std::size_t>(pointer.index) >= array.elements.size()) {
        return std::unexpected(ConstexprError(loc, "constexpr dereference out of bounds"));
    }
    return array.elements[static_cast<std::size_t>(pointer.index)];
}

class ConstexprEngine {
public:
    virtual ~ConstexprEngine() = default;
    ConstexprEngine(const Program& program, ConstexprLimits limits)
        : program_{program}, limits_{limits} {
        for (std::size_t i = 0; i < program_.functions.size(); ++i) {
            const std::string& function_name = program_.functions[i].name;
            if (functions_by_name_.contains(function_name)) {
                functions_by_name_.at(function_name).push_back(i);
            } else {
                std::vector<std::size_t> overload_indices{};
                overload_indices.push_back(i);
                functions_by_name_.emplace(function_name, std::move(overload_indices));
            }
        }
        for (std::size_t i = 0; i < program_.classes.size(); ++i) classes_by_name_.emplace(program_.classes[i].name, i);
        for (std::size_t i = 0; i < program_.structs.size(); ++i) structs_by_name_.emplace(program_.structs[i].name, i);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_root_expr(const Expr& expr) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return evaluate_expr(expr);
    }

    // [expr.const]/13: an immediate invocation is a call that *names* an
    // immediate function -- and which function a call names is settled by
    // overload resolution ([over.match]/3), never by scanning
    // declarations for the name. Asking the name alone made an ordinary
    // runtime call unwritable beside a `consteval` overload: with
    // `int f(int)` and `consteval int f(double)` declared, `f(1)` selects
    // `f(int)` and is a plain runtime call, yet it was force-folded and
    // then rejected with "immediate evaluation may only call
    // constexpr/consteval functions" -- in a plain `return f(1);`, in a
    // `static` local's initializer and in a namespace-scope
    // initializer alike, while clang compiles all three.
    //
    // The declaration scan survives only as the fast path: it avoids
    // evaluating arguments for the overwhelming majority of calls that
    // have no `consteval` overload at all, and it answers directly when
    // every arity-matching candidate is `consteval`, where resolution
    // has nothing else to choose. A *mixed* set is resolved for real,
    // through the same find_callable every other call in this file uses.
    [[nodiscard]] bool call_names_immediate_function(const Expr& expr) {
        if (expr.kind != ExprKind::Call || expr.lhs) return false;
        std::string suffix{};
        suffix += "::";
        suffix += expr.name;
        bool any_immediate = false;
        bool any_other = false;
        for (const Function& fn : program_.functions) {
            if (fn.name != expr.name && !fn.name.ends_with(suffix)) continue;
            if (fn.params.size() != expr.args.size()) continue;
            if (fn.eval_mode == FunctionEvalMode::Consteval) {
                any_immediate = true;
            } else {
                any_other = true;
            }
        }
        if (!any_immediate) return false;
        if (!any_other) return true;
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        std::vector<std::shared_ptr<Cell>> arg_values{};
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) {
            if (arg != nullptr && arg->kind == ExprKind::BracedInitList) {
                arg_values.push_back(nullptr);
                continue;
            }
            // An argument that is not itself constant-evaluable leaves
            // the question unanswerable here; keeping the immediate
            // reading preserves the diagnostic the call would otherwise
            // have produced instead of silently letting it through.
            auto arg_result = evaluate_expr(*arg);
            if (!arg_result.has_value()) return true;
            arg_values.push_back(std::move(arg_result).value());
        }
        OptionalFunctionRef callee =
            find_callable(expr.name, arg_values, expr.explicit_global_qualification, &expr.args);
        if (!callee.has_value()) return true;
        return callee->get().eval_mode == FunctionEvalMode::Consteval;
    }

    [[nodiscard]] std::expected<void, ConstexprError> validate_constexpr_locals(Function& fn) {
        if (!fn.body) return {};
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        std::vector<std::string> saved_namespace_path = enter_namespace(fn.namespace_path);
        frames_.emplace_back();
        auto result = validate_constexpr_stmt_tree(*fn.body);
        frames_.pop_back();
        leave_namespace(saved_namespace_path);
        return result;
    }

    // ch09 §9.1(1) leaves [dcl.constexpr] in force unchanged, so a
    // `constexpr` variable's initializer must be a constant expression --
    // at namespace scope exactly as inside a function. ch07 §7.1(2) makes
    // that required constant evaluation and §7.1(3) makes it well-formed
    // only if every necessary operation is permitted by §7.2 and none
    // listed in §7.3 is evaluated.
    //
    // Nothing performed that evaluation for a global. `resolve_global_-
    // constant` below has always been able to, but only *lazily*, when
    // some other constant expression named the global; a `constexpr`
    // global that nothing else mentioned was never evaluated on its own
    // account, so every rule -- division by zero, overflow, the
    // user-defined-destructor rule, the step budget, calling a
    // non-`constexpr` function -- went unenforced there, and the
    // initializer was then emitted as an ordinary *runtime* dynamic
    // initializer. `constexpr` at namespace scope promised compile-time
    // initialization and delivered neither the promise nor its check.
    // This is validate_constexpr_locals' missing counterpart: the
    // traversal existed for one arm and was never written for the other.
    //
    // Deliberately drives `resolve_global_constant` rather than
    // re-evaluating here, so the memoized result, the isolated frame
    // stack, the order-independence of one global initialized from a
    // later one, and the existing circular-dependency diagnostic all stay
    // single-sourced. The per-evaluation budgets are reset per global --
    // each initializer is its own required constant evaluation -- while
    // `string_storage_counter_` deliberately is not, since memoized cells
    // from an earlier global outlive this loop and their storage ids must
    // stay distinct.
    [[nodiscard]] std::expected<void, ConstexprError> validate_constexpr_global(GlobalVar& global) {
        if (global.decl == nullptr || !global.decl->is_constexpr || !global.decl->init) return {};
        steps_ = 0;
        call_depth_ = 0;
        std::vector<std::string> namespace_path = global.namespace_path;
        std::string name = global.decl->var_name;
        SourceLocation loc = global.decl->loc;
        auto value_result =
            resolve_global_constant(name, namespace_path, /*explicit_global_qualification=*/false, loc);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::shared_ptr<Cell> value = std::move(value_result).value();
        if (value == nullptr) return {};
        // Same best-effort folding as the local case: validation has
        // already succeeded, and a value that cannot be lowered back into
        // source-form AST simply keeps its original initializer.
        ignore_result(rewrite_expr_as_constant(*global.decl->init, value));
        return {};
    }

    // The namespace an unqualified name in the code currently being
    // evaluated is looked up from -- ch11 §11.5's ordinary rule that an
    // unqualified name is sought in the enclosing namespace and then
    // outwards. Saved and restored rather than pushed/popped, because a
    // call transfers lookup wholesale to the callee's namespace instead
    // of nesting inside the caller's.
    // Copies rather than moves: `std::move` of a *member* is one of the
    // constructs self-hosting does not yet accept (see the identical
    // `std::move(frames_)` below), and src/ is dual-compiled.
    [[nodiscard]] std::vector<std::string> enter_namespace(const std::vector<std::string>& namespace_path) {
        std::vector<std::string> saved = lookup_namespace_path_;
        lookup_namespace_path_ = namespace_path;
        return saved;
    }

    void leave_namespace(const std::vector<std::string>& saved) { lookup_namespace_path_ = saved; }

    [[nodiscard]] std::expected<std::uint64_t, ConstexprError> resolve_root_alignment_specs(const std::vector<AlignmentSpecifier>& specs, std::uint64_t natural_alignment,
                                               const std::string& what,
                                               const std::vector<std::string>& namespace_path) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        std::vector<std::string> saved_namespace_path = enter_namespace(namespace_path);
        auto result = resolve_alignment_specs(specs, natural_alignment, what);
        leave_namespace(saved_namespace_path);
        return result;
    }

    // ch05 §9.4: evaluates and validates a single array-bound
    // constant-expression (the `N` in `T name[N]`) -- required constant
    // evaluation, exactly like `alignas`'s own operand above. Rejects a
    // non-constant operand, a non-integral result, and a resolved value
    // that is not strictly greater than zero, each with its own clear
    // diagnostic (§9.4(4)-(5)). Does NOT touch `frames_`/step counters --
    // safe to call while already nested inside an in-progress evaluation
    // (e.g. from `validate_constexpr_stmt_tree`, which runs inside a
    // `frames_` scope pushed by `validate_constexpr_locals`). Use
    // `resolve_root_array_bound` instead at a true top-level call site.
    [[nodiscard]] std::expected<std::int64_t, ConstexprError> evaluate_and_validate_array_bound(const Expr& expr) {
        auto value_result = evaluate_expr(expr);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::shared_ptr<Cell> value = std::move(value_result).value();
        if (!is_integer_like(value->type)) {
            return std::unexpected(ConstexprError(expr.loc,
                                 "array bound must be a converted constant expression of type 'std::size_t'"));
        }
        auto raw_result = as_integer(value, expr.loc);
        if (!raw_result.has_value()) return std::unexpected(std::move(raw_result).error());
        std::int64_t raw = raw_result.value();
        if (raw <= 0) {
            std::string message{};
            message += "array bound must be greater than zero (got ";
            message += std::to_string(raw);
            message += ")";
            return std::unexpected(ConstexprError(expr.loc, message));
        }
        return raw;
    }

    // Top-level entry point: resets evaluation state (mirroring
    // `resolve_root_alignment_specs`) before evaluating. Only safe to call
    // when no other evaluation is already in progress on this engine.
    [[nodiscard]] std::expected<std::int64_t, ConstexprError> resolve_root_array_bound(const Expr& expr,
                                                                                       const std::vector<std::string>& namespace_path) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        std::vector<std::string> saved_namespace_path = enter_namespace(namespace_path);
        auto result = evaluate_and_validate_array_bound(expr);
        leave_namespace(saved_namespace_path);
        return result;
    }

    // ch05 §9.4: recursively resolves every not-yet-evaluated array bound
    // reachable from `type` (its own bound if `type` itself is an array,
    // then each dimension of a multi-dimensional array, and any array
    // type nested inside a pointer/reference/span/function signature),
    // replacing `array_size_expr` with the validated `array_size` in
    // place. Safe to call more than once on the same Type: a Type with no
    // pending `array_size_expr` (already resolved, or never an array in
    // the first place) is left untouched. Deliberately uses
    // `evaluate_and_validate_array_bound` (NOT `resolve_root_array_bound`):
    // this is called from `validate_constexpr_stmt_tree` while a `frames_`
    // scope is already active, so it must not reset the frame stack out
    // from under its caller.
    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds_in_type(Type& type) {
        switch (type.kind) {
            case TypeKind::Array:
                if (type.element) {
                    if (auto result = resolve_array_bounds_in_type(*type.element); !result.has_value()) return result;
                }
                if (type.array_size_expr) {
                    auto resolved_result = evaluate_and_validate_array_bound(*type.array_size_expr);
                    if (!resolved_result.has_value()) return std::unexpected(std::move(resolved_result).error());
                    type.array_size = std::move(resolved_result).value();
                    type.array_size_expr.reset();
                }
                return {};
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) return resolve_array_bounds_in_type(*type.pointee);
                return {};
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) {
                    if (auto result = resolve_array_bounds_in_type(*type.function_return); !result.has_value()) return result;
                }
                for (Type& param : type.function_params) {
                    if (auto result = resolve_array_bounds_in_type(param); !result.has_value()) return result;
                }
                return {};
            case TypeKind::Named:
                return {};
        }
        return {};
    }

    // ch05 §9.4 (local-constexpr-as-array-bound gap fix): the four methods
    // below let AlignmentResolver::resolve_array_bounds() -- a separate,
    // earlier, non-executing pre-pass that must resolve every array bound
    // in the program before this class's own `validate_constexpr_locals`
    // above ever runs (see that other pass's own comment for why it has to
    // stay distinct and narrower) -- track each local `constexpr` (or
    // constant-initialized `const`) declaration into this SAME `frames_`
    // stack, in declaration order, as it walks one function body's
    // statements. Without this, a local array's bound expression could see
    // a *global* constexpr (resolve_global_constant doesn't consult
    // frames_ at all) but not an earlier *local* constexpr from the same
    // function, even though that same local constexpr was already usable
    // as an `alignas` operand via validate_constexpr_stmt_tree's own,
    // identical frame bookkeeping. Mirrors validate_constexpr_locals/
    // validate_constexpr_stmt_tree's own scope shape exactly (one root
    // frame per function, one nested frame per block/if/while branch), so
    // both passes agree on ordinary C++ block-scoping: a constexpr from an
    // enclosing or the same block is visible to a later array bound, one
    // from a later statement or an unrelated sibling block is not, and an
    // inner declaration correctly shadows an outer one of the same name.
    void begin_local_array_bound_scope(const std::vector<std::string>& namespace_path) {
        saved_array_bound_namespace_path_ = enter_namespace(namespace_path);
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        frames_.emplace_back();
    }

    void end_local_array_bound_scope() {
        frames_.pop_back();
        leave_namespace(saved_array_bound_namespace_path_);
    }

    void push_local_array_bound_scope() { frames_.emplace_back(); }
    void pop_local_array_bound_scope() { frames_.pop_back(); }

    // Mirrors validate_constexpr_stmt_tree's own VarDecl handling below
    // (is_constexpr required-strict; is_const-with-init best-effort,
    // failures silently ignored) but deliberately skips that method's
    // extra rewrite_expr_as_constant step -- an unrelated AST-
    // simplification left exclusively to validate_constexpr_locals/run(),
    // unchanged. This only needs to make the local's own *value* available
    // in the current innermost frame (via execute_stmt's existing
    // `frames_.back()[var_name] = ...` binding) so a later sibling/nested
    // array-bound expression in the same function can look it up, exactly
    // like it already could as an `alignas` operand.
    [[nodiscard]] std::expected<void, ConstexprError> bind_local_constant_for_array_bounds(const Stmt& stmt) {
        if (stmt.is_constexpr) {
            auto result = execute_stmt(stmt, named_type("void"));
            if (!result.has_value()) return std::unexpected(std::move(result).error());
        } else if (stmt.is_const && (stmt.init || stmt.has_ctor_args)) {
            // Best-effort: a failing local `const` initializer is not (yet)
            // usable as an array bound, but that's fine here -- silently
            // tolerated exactly like the original catch(const ConstexprError&) {}.
            ignore_result(execute_stmt(stmt, named_type("void")));
        }
        return {};
    }

    // ch05 §9.4(6): while a struct/class's own fields are being resolved
    // (AlignmentResolver::resolve_struct/resolve_class), its own type is
    // not yet complete -- marks/unmarks `name` so that evaluating a
    // `sizeof`/`alignof` naming it during that window is rejected with a
    // clear diagnostic instead of silently computing a bogus (typically
    // zero) size from the still-in-progress definition.
    void mark_type_incomplete(const std::string& name) { incomplete_type_names_.insert(name); }
    void mark_type_complete(const std::string& name) { incomplete_type_names_.erase(name); }

private:
    const Program& program_;
    ConstexprLimits limits_{};
    int steps_ = 0;
    int call_depth_ = 0;
    // Frame address of the outermost call_function of the current
    // evaluation, captured when call_depth_ is still 0; see that function
    // for why the engine measures bytes and not only levels.
    const char* stack_base_ = nullptr;
    int string_storage_counter_ = 0;
    std::vector<std::unordered_map<std::string, Binding>> frames_{};
    std::unordered_map<std::string, std::vector<std::size_t>> functions_by_name_{};
    std::unordered_map<std::string, std::size_t> classes_by_name_{};
    std::unordered_map<std::string, std::size_t> structs_by_name_{};
    std::unordered_set<std::string> incomplete_type_names_{};
    // ch05 §9.4(8)/06-constant-evaluation.md: a required constant
    // expression (an array bound, an `alignas` operand, ...) may name a
    // global `constexpr` variable (e.g. `constexpr int kBufferSize = 64;
    // char buf[kBufferSize];`) -- unlike an ordinary local, a global is
    // never pushed onto frames_ by any statement-execution path, so
    // lookup_binding falls back to these when a plain frame-stack lookup
    // finds nothing.
    //
    // *Name resolution* -- which global a spelling refers to -- is not
    // asked here at all: `find_visible_global` in ast.cppm answers it,
    // as it already does for movecheck and codegen. These two answer the
    // separate question of what has already been *evaluated*:
    // resolved_global_constants_ memoizes each global's own once-
    // evaluated value (a global constexpr initializer is evaluated at
    // most once, no matter how many other constant expressions go on to
    // reference it); globals_resolving_ detects `constexpr int A = B;
    // constexpr int B = A;`-style circular dependencies instead of
    // recursing forever. Both are keyed on the resolved global's own
    // qualified `var_name`, never on the spelling at a use site.
    std::unordered_map<std::string, std::shared_ptr<Cell>> resolved_global_constants_{};
    std::unordered_set<std::string> globals_resolving_{};
    // ch11 §11.5: the namespace an unqualified name is currently looked
    // up from, walked outwards by find_visible_global/find_callable. Set
    // from the declaration whose code is being evaluated -- a global's
    // own namespace while its initializer runs, a function's own while
    // its body runs -- so lookup never depends on which reference
    // happened to trigger the evaluation.
    std::vector<std::string> lookup_namespace_path_{};
    std::vector<std::string> saved_array_bound_namespace_path_{};

    // ch05 §9.4(6): `struct Self { char buf[sizeof(Self)]; };` -- Self is
    // incomplete at this point, so evaluating its size/alignment must be
    // rejected rather than silently computed from a partially-resolved
    // definition (see mark_type_incomplete/mark_type_complete above).
    [[nodiscard]] std::expected<void, ConstexprError> reject_if_incomplete(const Type& queried_type, const SourceLocation& loc, const char* op) const {
        if (queried_type.kind == TypeKind::Named && incomplete_type_names_.contains(queried_type.name)) {
            std::string message{};
            message += "cannot apply '";
            message += op;
            message += "' to '";
            message += queried_type.name;
            message += "': it is still an incomplete type at this point";
            return std::unexpected(ConstexprError(loc, message));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> tick(const SourceLocation& loc, const char* what) {
        ++steps_;
        if (steps_ > limits_.max_steps) {
            std::string message{};
            message += "constexpr evaluation exceeded step budget while ";
            message += what;
            return std::unexpected(ConstexprError(loc, message));
        }
        return {};
    }

    [[nodiscard]] static bool is_power_of_two(std::uint64_t value) {
        // Spelled with division rather than `value & (value - 1)`: scpp has no
        // bitwise operators, the same reason ast.cppm's align_up is written as
        // `((value + align - 1) / align) * align`.
        if (value == 0) return false;
        while (value > 1) {
            if (value / 2 * 2 != value) return false;
            value = value / 2;
        }
        return true;
    }

    [[nodiscard]] std::expected<std::uint64_t, ConstexprError> evaluate_alignment_operand(const AlignmentSpecifier& spec) {
        if (spec.operand_is_type) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, spec.type);
            if (!layout.has_value()) {
                return std::unexpected(ConstexprError(spec.loc, "cannot apply 'alignas' to this type in this version"));
            }
            return layout->abi_align_bytes;
        }
        if (!spec.expr) {
            return std::unexpected(ConstexprError(spec.loc, "internal error: malformed alignas operand"));
        }
        auto value_result = evaluate_expr(*spec.expr);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::shared_ptr<Cell> value = std::move(value_result).value();
        if (!is_integer_like(value->type)) {
            return std::unexpected(ConstexprError(spec.loc, "'alignas' requires an integral constant expression"));
        }
        auto raw_result = as_integer(value, spec.loc);
        if (!raw_result.has_value()) return std::unexpected(std::move(raw_result).error());
        std::int64_t raw = raw_result.value();
        if (raw < 0) {
            return std::unexpected(ConstexprError(spec.loc, "'alignas' requires a non-negative alignment value"));
        }
        return static_cast<std::uint64_t>(raw);
    }

    [[nodiscard]] std::expected<std::uint64_t, ConstexprError> resolve_alignment_specs(const std::vector<AlignmentSpecifier>& specs,
                                                        std::uint64_t natural_alignment,
                                                        const std::string& what) {
        std::uint64_t strictest = 0;
        for (const AlignmentSpecifier& spec : specs) {
            auto requested_result = evaluate_alignment_operand(spec);
            if (!requested_result.has_value()) return std::unexpected(std::move(requested_result).error());
            std::uint64_t requested = requested_result.value();
            if (requested == 0) continue;
            if (!is_power_of_two(requested)) {
                return std::unexpected(ConstexprError(spec.loc, "'alignas' requires a positive power-of-two alignment"));
            }
            if (requested < natural_alignment) {
                std::string message{};
                message += "'alignas' requests alignment ";
                message += std::to_string(static_cast<std::size_t>(requested));
                message += ", which is less strict than the natural alignment ";
                message += std::to_string(static_cast<std::size_t>(natural_alignment));
                message += " of ";
                message += what;
                return std::unexpected(ConstexprError(spec.loc, message));
            }
            strictest = std::max(strictest, requested);
        }
        // An alignment no stricter than the natural one needs no explicit
        // resolved alignment, which this reports as zero. The zero is a
        // named local because scpp does not unify an untyped literal with
        // the other arm's type across a conditional expression.
        std::uint64_t resolved = 0;
        if (strictest > natural_alignment) resolved = strictest;
        return resolved;
    }

    [[nodiscard]] std::expected<void, ConstexprError> validate_constexpr_stmt_tree(Stmt& stmt) {
        if (auto result = tick(stmt.loc, "checking a constexpr local declaration"); !result.has_value()) return result;
        switch (stmt.kind) {
            case StmtKind::VarDecl: {
                // ch05 §9.4: a local variable's own array bound (e.g.
                // `char buf[sizeof(int)];`) must be resolved before
                // anything below reads `stmt.type`'s layout (its
                // `alignas`, if any) or codegen ever sees this
                // declaration.
                if (auto result = resolve_array_bounds_in_type(stmt.type); !result.has_value()) return result;
                if (!stmt.alignment_specs.empty()) {
                    std::optional<TypeLayoutInfo> layout = layout_of_type(program_, stmt.type);
                    if (!layout.has_value()) {
                        return std::unexpected(ConstexprError(stmt.loc, "cannot apply 'alignas' to this variable type in this version"));
                    }
                    std::string alignment_context{};
                    alignment_context += "variable '";
                    alignment_context += stmt.var_name;
                    alignment_context += "'";
                    auto alignment_result =
                        resolve_alignment_specs(stmt.alignment_specs, layout->abi_align_bytes, alignment_context);
                    if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
                    stmt.resolved_alignment = alignment_result.value();
                }
                if (stmt.is_constexpr) {
                    if (auto result = execute_stmt(stmt, named_type("void")); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                    if (stmt.init) {
                        auto binding_result = lookup_binding(stmt.var_name, stmt.loc);
                        if (!binding_result.has_value()) return std::unexpected(std::move(binding_result).error());
                        // Some valid constant-expression results (notably
                        // richer object values) still cannot be lowered
                        // back into source-form AST here. Validation has
                        // already succeeded, so keep the original
                        // initializer in those cases.
                        ignore_result(rewrite_expr_as_constant(*stmt.init, binding_result.value().cell));
                    }
                } else if (stmt.is_const && (stmt.init || stmt.has_ctor_args)) {
                    // Best-effort, matching bind_local_constant_for_array_bounds
                    // above and the original catch(const ConstexprError&) {}: a
                    // local `const` whose initializer is not a constant
                    // expression simply does not become one, which is not an
                    // error here.
                    ignore_result(execute_stmt(stmt, named_type("void")));
                }
                return {};
            }
            case StmtKind::Block: {
                frames_.emplace_back();
                std::expected<void, ConstexprError> result{};
                for (StmtPtr& nested : stmt.statements) {
                    result = validate_constexpr_stmt_tree(*nested);
                    if (!result.has_value()) break;
                }
                frames_.pop_back();
                return result;
            }
            case StmtKind::If: {
                if (stmt.then_branch) {
                    frames_.emplace_back();
                    auto result = validate_constexpr_stmt_tree(*stmt.then_branch);
                    frames_.pop_back();
                    if (!result.has_value()) return result;
                }
                if (stmt.else_branch) {
                    frames_.emplace_back();
                    auto result = validate_constexpr_stmt_tree(*stmt.else_branch);
                    frames_.pop_back();
                    if (!result.has_value()) return result;
                }
                return {};
            }
            case StmtKind::While: {
                if (stmt.then_branch) {
                    frames_.emplace_back();
                    auto result = validate_constexpr_stmt_tree(*stmt.then_branch);
                    frames_.pop_back();
                    if (!result.has_value()) return result;
                }
                return {};
            }
            case StmtKind::Switch: {
                for (SwitchCase& switch_case : stmt.switch_cases) {
                    frames_.emplace_back();
                    std::expected<void, ConstexprError> result{};
                    for (StmtPtr& nested : switch_case.statements) {
                        result = validate_constexpr_stmt_tree(*nested);
                        if (!result.has_value()) break;
                    }
                    frames_.pop_back();
                    if (!result.has_value()) return result;
                }
                return {};
            }
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough:
            case StmtKind::ExprStmt:
                return {};
        }
        return {};
    }

    [[nodiscard]] std::shared_ptr<Cell> clone_cell(const std::shared_ptr<Cell>& cell) {
        auto copy = std::make_shared<Cell>();
        copy->type = cell->type;
        // Was a std::visit over the variant with an `if constexpr` chain;
        // a plain switch on the tag says the same thing directly, and is
        // the only form available without `if constexpr`.
        switch (cell->data.kind) {
            case CellKind::Empty:
            case CellKind::Integer:
            case CellKind::Double:
            case CellKind::Bool:
            case CellKind::Pointer:
            case CellKind::Span:
                copy->data = cell->data;
                break;
            case CellKind::Object: {
                ObjectValue object_copy{};
                object_copy.type_name = cell->data.object.type_name;
                for (const ObjectField& field : cell->data.object.fields) {
                    object_copy.add_field(field.name, clone_cell(field.cell));
                }
                copy->data.set_object(std::move(object_copy));
                break;
            }
            case CellKind::Array: {
                ArrayValue array_copy{};
                array_copy.element_type = cell->data.array.element_type;
                for (const std::shared_ptr<Cell>& element : cell->data.array.elements) {
                    array_copy.elements.push_back(clone_cell(element));
                }
                copy->data.set_array(std::move(array_copy));
                break;
            }
        }
        return copy;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_scalar_cell(Type type, std::int64_t value) {
        auto cell = std::make_shared<Cell>();
        cell->type = std::move(type);
        cell->data.set_integer(value);
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_double_cell(double value) {
        return make_float_cell_as(named_type("double"), value);
    }

    // A floating cell carries its own type rather than always claiming
    // `double`, so that a `float32_t` value stays a `float32_t` through
    // arithmetic and assignment. The value is rounded to the type's own
    // precision on the way in: constant evaluation of a 32-bit float has
    // to produce the same bits codegen's fptrunc would, or a constexpr
    // and a runtime computation of the same expression would disagree.
    [[nodiscard]] std::shared_ptr<Cell> make_float_cell_as(const Type& type, double value) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        cell->data.set_double(narrow_float_value(type, value));
        return cell;
    }

    [[nodiscard]] double narrow_float_value(const Type& type, double value) const {
        if (type.kind != TypeKind::Named) return value;
        if (scpp::scalar_bit_width(std::string_view{type.name}, scpp::host_pointer_bit_width()) == 32) {
            return static_cast<double>(static_cast<float>(value));
        }
        return value;
    }

    // Builds the cell for a literal that has adopted `target`. The
    // caller has already established adoption is legal, so this does not
    // re-check the value's range: `literal_adopts_type` *is* the answer
    // to that question for a literal, and asking again through
    // `integer_bounds_for_type` gives a different one for a 64-bit
    // unsigned target. Such a target deliberately accepts a negative
    // literal, because a literal above INT64_MAX has already wrapped by
    // the time it arrives and `-5` and `18446744073709551611` are the
    // same int64 carrier, while `scalar_value_range` reports {0,
    // INT64_MAX}. Running both checks made `constexpr size_t v = -5;` an
    // overflow error against a move checker and a codegen that both
    // accept it -- the same two-layer disagreement this path exists to
    // remove. Arithmetic keeps the bounds check, which is right: scpp
    // traps on unsigned underflow at runtime, so rejecting `size_t v = a
    // - 5;` at compile time is the same answer given earlier.
    //
    // An integer literal adopting a floating type -- `double d = 5;`,
    // which move checking accepts -- becomes a floating cell.
    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> make_adopted_literal_cell(const Expr& literal, const Type& target) {
        if (literal.kind == ExprKind::Unary && literal.lhs != nullptr) {
            return make_adopted_literal_value_cell(*literal.lhs, target, /*negated=*/true);
        }
        return make_adopted_literal_value_cell(literal, target, /*negated=*/false);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> make_adopted_literal_value_cell(const Expr& value_expr, const Type& target,
                                                                            bool negated) {
        if (value_expr.kind == ExprKind::FloatLiteral) {
            double value = negated ? -value_expr.float_value : value_expr.float_value;
            return make_float_cell_as(target, value);
        }
        std::int64_t value = negated ? -value_expr.int_value : value_expr.int_value;
        if (is_floating_like(target)) return make_float_cell_as(target, static_cast<double>(value));
        return make_scalar_cell(target, value);
    }

    [[nodiscard]] std::shared_ptr<Cell> make_bool_cell(bool value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("bool");
        cell->data.set_bool(value);
        return cell;
    }

    // `nullptr` (ch06 §6). A Pointer cell whose PointerValue is
    // default-constructed: its `storage` is a null shared_ptr, which is
    // exactly the "points at no object" state every pointer consumer
    // here already tests for before dereferencing.
    [[nodiscard]] std::shared_ptr<Cell> make_nullptr_cell() {
        auto cell = std::make_shared<Cell>();
        cell->type = nullptr_named_type();
        cell->data.set_pointer(PointerValue{});
        return cell;
    }

    [[nodiscard]] std::expected<std::vector<ClassField>, ConstexprError> collect_class_fields(const ClassDef& def) {
        std::vector<ClassField> fields{};
        if (auto base = def.direct_ordinary_base(); base.has_value()) {
            if (!classes_by_name_.contains(base->get().base_type.name)) {
                std::string message{};
                message += "missing constexpr class definition for base class '";
                message += base->get().base_type.name;
                message += "'";
                return std::unexpected(ConstexprError(SourceLocation{}, message));
            }
            const ClassDef& base_def = program_.classes[classes_by_name_.at(base->get().base_type.name)];
            auto base_fields_result = collect_class_fields(base_def);
            if (!base_fields_result.has_value()) return std::unexpected(std::move(base_fields_result).error());
            std::vector<ClassField> base_fields = std::move(base_fields_result).value();
            fields.insert(fields.end(), base_fields.begin(), base_fields.end());
        }
        fields.insert(fields.end(), def.fields.begin(), def.fields.end());
        return fields;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> make_default_cell(const Type& type, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        // A Cell is storage, and this evaluator tracks whether storage may
        // be written through LValue::read_only/Binding::read_only -- its
        // own, complete model of constness, which is what rejects `c = 1;`
        // and `f(c)` for a mutable-reference parameter here. The declared
        // type's top-level `const` is the same fact spelled a second way,
        // and carrying it on the cell too would make every `constexpr int
        // x = 5;` a type mismatch against the plain `int` value being
        // stored ([conv.lval]: the value has no qualifier). Cv-qualifiers
        // *below* the top level -- `const int*`'s pointee -- are part of
        // the type proper and stay.
        cell->type.is_const_qualified = false;
        switch (type.kind) {
            case TypeKind::Named: {
                if (is_integral_named_type(type.name) && type.name != "bool") {
                    cell->data.set_integer(0);
                    return cell;
                }
                if (type.name == "bool") {
                    cell->data.set_bool(false);
                    return cell;
                }
                if (is_floating_like(type)) {
                    cell->data.set_double(0.0);
                    return cell;
                }
                if (type.name == "void") {
                    cell->data.set_empty();
                    return cell;
                }
                if (structs_by_name_.contains(type.name)) {
                    const StructDef& struct_def = program_.structs[structs_by_name_.at(type.name)];
                    ObjectValue object{};
                    object.type_name = type.name;
                    for (const StructField& field : struct_def.fields) {
                        auto field_result = make_default_cell(field.type, loc);
                        if (!field_result.has_value()) return std::unexpected(std::move(field_result).error());
                        object.add_field(field.name, std::move(field_result).value());
                    }
                    cell->data.set_object(std::move(object));
                    return cell;
                }
                if (classes_by_name_.contains(type.name)) {
                    const ClassDef& class_def = program_.classes[classes_by_name_.at(type.name)];
                    ObjectValue object{};
                    object.type_name = type.name;
                    auto fields_result = collect_class_fields(class_def);
                    if (!fields_result.has_value()) return std::unexpected(std::move(fields_result).error());
                    for (const ClassField& field : fields_result.value()) {
                        if (field.type.kind == TypeKind::Reference && field.type.pointee) {
                            auto field_result = make_default_cell(*field.type.pointee, loc);
                            if (!field_result.has_value()) return std::unexpected(std::move(field_result).error());
                            object.add_field(field.name, std::move(field_result).value());
                        } else {
                            auto field_result = make_default_cell(field.type, loc);
                            if (!field_result.has_value()) return std::unexpected(std::move(field_result).error());
                            object.add_field(field.name, std::move(field_result).value());
                        }
                    }
                    cell->data.set_object(std::move(object));
                    return cell;
                }
                std::string message{};
                message += "type '";
                message += type.name;
                message += "' is not supported during constant evaluation";
                return std::unexpected(ConstexprError(loc, message));
            }
            case TypeKind::Pointer:
                cell->data.set_pointer(PointerValue{});
                return cell;
            case TypeKind::Array: {
                if (!type.element) return std::unexpected(ConstexprError(loc, "malformed array type in constexpr evaluator"));
                ArrayValue array{};
                array.element_type = *type.element;
                for (std::int64_t i = 0; i < type.array_size; ++i) {
                    auto element_result = make_default_cell(*type.element, loc);
                    if (!element_result.has_value()) return std::unexpected(std::move(element_result).error());
                    array.elements.push_back(std::move(element_result).value());
                }
                cell->data.set_array(std::move(array));
                return cell;
            }
            case TypeKind::Reference:
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                return std::unexpected(ConstexprError(loc, "references, functions and function pointers are not supported during constant evaluation"));
            case TypeKind::Span:
                if (type.is_mutable_ref) {
                    return std::unexpected(ConstexprError(loc, "mutable std::span<T> is not supported during constant evaluation"));
                }
                cell->data.set_span(SpanValue{});
                return cell;
        }
        return std::unexpected(ConstexprError(loc, "unsupported constexpr type"));
    }

    [[nodiscard]] std::expected<Binding, ConstexprError> lookup_binding(const std::string& name, const SourceLocation& loc,
                                                                       bool explicit_global_qualification = false) {
        // std::vector has no rbegin()/rend() yet -- walk backwards (innermost
        // frame first) by index instead, as parser.cppm already does.
        for (std::size_t i = frames_.size(); i > 0; --i) {
            const std::unordered_map<std::string, Binding>& frame = frames_[i - 1];
            if (frame.contains(name)) return frame.at(name);
        }
        auto global_result =
            resolve_global_constant(name, lookup_namespace_path_, explicit_global_qualification, loc);
        if (!global_result.has_value()) return std::unexpected(std::move(global_result).error());
        if (std::shared_ptr<Cell> global_value = std::move(global_result).value(); global_value != nullptr) {
            return Binding{global_value, /*read_only=*/true};
        }
        std::string message{};
        message += "expression is not a constant expression: identifier '";
        message += name;
        message += "' is not available";
        // Naming *why* rather than only "not available", because the two
        // reasons need different fixes and the old message named
        // neither: a runtime variable has to be made `const`/`constexpr`,
        // while a variable that already is one but whose initializer is
        // not itself a constant expression has to have that initializer
        // fixed instead.
        std::optional<std::size_t> index =
            find_visible_global_index(program_, lookup_namespace_path_, name, explicit_global_qualification);
        if (index.has_value()) {
            const GlobalVar& global = program_.globals[*index];
            if (global.decl != nullptr) {
                message += " (the global '";
                message += name;
                message += "' declared at line ";
                message += std::to_string(static_cast<std::int64_t>(global.decl->loc.line));
                if (!global.decl->is_const && !global.decl->is_constexpr && !global.decl->type.is_const_qualified) {
                    message += " is neither 'const' nor 'constexpr', so its value is not known until runtime)";
                } else if (!global.decl->init) {
                    message += " has no initializer to evaluate)";
                } else {
                    message += " has an initializer that is not itself a constant expression)";
                }
            }
        }
        return std::unexpected(ConstexprError(loc, message));
    }

    // ch05 §9.4(8): a required constant expression may name a global
    // `constexpr` variable (e.g. `constexpr int kBufferSize = 64; char
    // buf[kBufferSize];`, straight from the spec's own accepted-examples
    // list) -- and, by §7.1(1)'s adoption of [expr.const] unchanged, a
    // `const` one with an initializer too ([expr.const]/3's
    // "usable in constant expressions"). §9.4(5)'s own Note says as much
    // from the other side: "a *non-`const`*, non-`constexpr` local
    // variable ... is not usable in a constant expression ... and
    // therefore cannot be read by an array bound".
    //
    // The `const` arm is *best-effort*, exactly as
    // bind_local_constant_for_array_bounds and
    // validate_constexpr_stmt_tree's VarDecl case already make it for a
    // local: a `const` global whose initializer is not a constant
    // expression simply does not become one, which is not an error at the
    // declaration -- only at a use site that required a constant. Making
    // the global rule anything other than the local rule is what made
    // `const int n = 3; int a[n];` compile inside a function and fail at
    // namespace scope.
    //
    // Returns nullptr for any name that isn't such a global (a plain
    // runtime global, or no global at all), so lookup_binding's
    // existing "not usable" diagnostic still fires for
    // those. Evaluates the global's own initializer, at most once
    // (memoized in resolved_global_constants_), in a completely isolated
    // frame stack: a global initializer must only ever see other
    // globals/functions, never whatever local variables happen to be
    // live in the caller that triggered this lookup.
    //
    // Name resolution is `find_visible_global`'s, not this file's: the
    // evaluator used to match a global by exact string against its own
    // `globals_by_name_` index, which has no notion of a namespace at
    // all. That made a namespaced global reachable only by its fully
    // qualified spelling, and -- worse -- silently bound an unqualified
    // name to a same-named global in an *enclosing* namespace, so
    // `namespace a { constexpr int T = 8; ... T ... }` evaluated to a
    // global `::T` if one existed. ast.cppm already exported the correct
    // progressive-outward walk, and movecheck and codegen already used
    // it; nothing about this question needed a second answer.
    //
    // The memo and the cycle set are keyed on the *resolved* global's own
    // qualified `var_name` rather than on the spelling at the use site,
    // so `a::T` and `b::T` cannot collide and the same global reached by
    // two different spellings is still evaluated once.
    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError>
    resolve_global_constant(const std::string& name, const std::vector<std::string>& namespace_path,
                            bool explicit_global_qualification, const SourceLocation& loc) {
        std::optional<std::size_t> index =
            find_visible_global_index(program_, namespace_path, name, explicit_global_qualification);
        if (!index.has_value()) return nullptr;
        const GlobalVar& global = program_.globals[*index];
        if (global.decl == nullptr || !global.decl->init) return nullptr;
        bool is_required = global.decl->is_constexpr;
        bool is_const_global = global.decl->is_const || global.decl->type.is_const_qualified;
        if (!is_required && !is_const_global) return nullptr;
        std::string key = global.decl->var_name;
        if (resolved_global_constants_.contains(key)) return resolved_global_constants_.at(key);
        if (globals_resolving_.contains(key)) {
            std::string message{};
            message += "constant expression circularly depends on global constant variable '";
            message += key;
            message += "'";
            return std::unexpected(ConstexprError(loc, message));
        }
        globals_resolving_.insert(key);
        std::vector<std::unordered_map<std::string, Binding>> saved_frames = std::move(frames_);
        // spec §6.2(4): the move above already left `frames_` moved-out,
        // so it has to be given a value again before the evaluation
        // below reads it -- `.clear()` was a *use* of a moved-out object.
        frames_ = std::vector<std::unordered_map<std::string, Binding>>{};
        // A global's initializer is looked up from the namespace the
        // global itself was declared in, never from wherever the
        // reference that triggered this resolution happened to sit.
        std::vector<std::string> saved_namespace_path = enter_namespace(global.namespace_path);
        auto value_result = evaluate_expr_in_context(*global.decl->init, &global.decl->type);
        leave_namespace(saved_namespace_path);
        frames_ = std::move(saved_frames);
        globals_resolving_.erase(key);
        if (!value_result.has_value()) {
            // Best-effort for `const`, required-strict for `constexpr` --
            // the local rule, applied to the global.
            if (!is_required) return nullptr;
            return std::unexpected(std::move(value_result).error());
        }
        std::shared_ptr<Cell> value = std::move(value_result).value();
        resolved_global_constants_.emplace(key, value);
        return value;
    }

    // An enumeration's value is an integer of its underlying type
    // ([dcl.enum]/8), so an enum-typed cell answers this question too.
    // switch_match_key used to be a second answer to it, reading
    // `cell->data.int_value` itself for exactly the enum case this
    // rejected -- so `switch` could read an enumerator's value while
    // `static_cast<std::int64_t>(E::B)` could not.
    [[nodiscard]] std::expected<std::int64_t, ConstexprError> as_integer(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_enum_like(cell->type)) return cell->data.int_value;
        if (!is_integer_like(cell->type)) {
            return std::unexpected(ConstexprError(loc, "expected an integer-like constexpr value"));
        }
        if (is_named_type(cell->type, "bool")) return cell->data.bool_value ? static_cast<std::int64_t>(1) : static_cast<std::int64_t>(0);
        return cell->data.int_value;
    }

    [[nodiscard]] std::expected<double, ConstexprError> as_double(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_floating_like(cell->type)) return cell->data.double_value;
        if (is_integer_like(cell->type)) {
            auto result = as_integer(cell, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return static_cast<double>(result.value());
        }
        return std::unexpected(ConstexprError(loc, "expected a numeric constexpr value"));
    }

    [[nodiscard]] std::expected<bool, ConstexprError> as_bool(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_named_type(cell->type, "bool")) return cell->data.bool_value;
        if (is_integer_like(cell->type)) {
            auto result = as_integer(cell, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return result.value() != 0;
        }
        // A floating operand had no answer here, so
        // `constexpr bool b = static_cast<bool>(1.5);` failed against a
        // runtime `static_cast<bool>` that accepts it and yields true.
        // `!= 0` is the same test codegen emits, and it gives NaN the
        // same answer (true) that an fcmp-one against zero does.
        if (is_floating_like(cell->type)) return cell->data.double_value != 0.0;
        return std::unexpected(ConstexprError(loc, "expected a boolean constexpr value"));
    }

    [[nodiscard]] bool is_enum_like(const Type& type) {
        if (type.kind != TypeKind::Named) return false;
        for (const EnumDef& def : program_.enums) {
            if (def.name == type.name) return true;
        }
        return false;
    }

    [[nodiscard]] std::expected<std::int64_t, ConstexprError> switch_match_key(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_integer_like(cell->type) || is_enum_like(cell->type)) return as_integer(cell, loc);
        return std::unexpected(ConstexprError(loc, "switch requires an integral or enum constexpr value"));
    }

    // The range of values this engine's int64 carrier may hold for a
    // place of this type. Delegates to `scpp.ast`'s
    // `scalar_value_range`, so constant evaluation and movecheck's
    // literal-range check share one derivation from one model rather
    // than each carrying its own table -- see `scalar_type_info`.
    //
    // The table this replaced had drifted twice: it gave `char` the
    // range {0, 255} (the only part of the compiler that thought `char`
    // unsigned), and it omitted `uint32_t` entirely, so a
    // constant-evaluated `uint32_t` was bounded as a signed 64-bit
    // integer and would silently accept a negative value.
    //
    // Anything with no scalar range of its own -- an enum, or a floating
    // type reached through an integer path -- keeps the full 64-bit
    // signed range, which is what the old table's default arm gave it.
    [[nodiscard]] IntegerBounds integer_bounds_for_type(const Type& type) const {
        if (type.kind != TypeKind::Named) return IntegerBounds{int64_min_value, int64_max_value};
        std::optional<ScalarValueRange> range = scalar_value_range(std::string_view{type.name}, host_pointer_bit_width());
        if (!range.has_value()) return IntegerBounds{int64_min_value, int64_max_value};
        return IntegerBounds{range->min_value, range->max_value};
    }

    [[nodiscard]] std::expected<void, ConstexprError> checked_assign_integer(const std::shared_ptr<Cell>& target, std::int64_t value, const SourceLocation& loc) {
        if (is_named_type(target->type, "bool")) {
            target->data.set_bool(value != 0);
            return {};
        }
        IntegerBounds bounds = integer_bounds_for_type(target->type);
        if (value < bounds.min_value || value > bounds.max_value) {
            return std::unexpected(ConstexprError(loc, "constexpr integer overflow"));
        }
        target->data.set_integer(value);
        return {};
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> make_checked_int_cell(std::int64_t value, const SourceLocation& loc) {
        return make_checked_int_cell_as(named_type("int"), value, loc);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> make_checked_int_cell_as(const Type& type, std::int64_t value, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        auto result = checked_assign_integer(cell, value, loc);
        if (!result.has_value()) return std::unexpected(std::move(result).error());
        return cell;
    }

    [[nodiscard]] std::expected<void, ConstexprError> copy_into(const std::shared_ptr<Cell>& target, const std::shared_ptr<Cell>& source, const SourceLocation& loc) {
        // Top-level const is the *object's*, not the value's
        // ([conv.lval]/[dcl.init]) -- see
        // types_equal_ignoring_top_level_const. A `constexpr`/`const`
        // declaration's cell carries the qualifier its declared type now
        // records, and requiring the incoming value to carry it too would
        // reject every `constexpr int x = 5;` in the language.
        if (!types_equal_ignoring_top_level_const(target->type, source->type)) {
            return std::unexpected(ConstexprError(loc, "constexpr assignment requires exactly matching types"));
        }
        std::shared_ptr<Cell> cloned = clone_cell(source);
        target->data = std::move(cloned->data);
        return {};
    }

    // [dcl.init.string]/1 during constant evaluation, the evaluator's
    // counterpart of Codegen::try_initialize_array_from_string_literal.
    // ch06 §6.1(4.1)/(4.2) require the implementation to support both
    // string-literal objects and fixed-size arrays here, and a
    // `constexpr` program is the same program as its runtime twin: if
    // only one of the two implementations knows this rule they disagree
    // about what the program means.
    //
    // The literal's bytes are written into the array's own element cells
    // rather than the cell being replaced wholesale, because a
    // string-literal *expression* evaluates to a `const char*` pointing
    // at separate storage (make_string_literal_pointer) -- which is the
    // decayed value, not the initialization. Copying that pointer in is
    // exactly what the old path did, and it is why the evaluator reported
    // "constexpr assignment requires exactly matching types".
    [[nodiscard]] std::expected<bool, ConstexprError> try_initialize_array_cell_from_string_literal(
        const std::shared_ptr<Cell>& cell, const Type& type, const Expr& expr, const SourceLocation& loc) {
        if (!string_literal_initializes_char_array(type, expr)) return false;
        if (!string_literal_fits_array(type.array_size, expr.name.size())) {
            return std::unexpected(ConstexprError(loc, describe_string_literal_too_long_for_array(type, expr.name.size())));
        }
        if (!cell->data.is_array()) {
            return std::unexpected(ConstexprError(loc, "internal error: expected array storage for an array initializer"));
        }
        std::vector<std::shared_ptr<Cell>>& elements = cell->data.array.elements;
        for (std::size_t i = 0; i < elements.size(); ++i) {
            // Elements past the literal (and its terminating null) are
            // value-initialized -- [dcl.init.string]/1's "the remaining
            // elements are value-initialized" -- which for `char` is 0,
            // the same value the byte loop below writes for them.
            //
            // The cast through std::int8_t matches
            // make_string_literal_pointer: `char` is signed (ch06 §16.1),
            // so a 0xC8 byte is -56, the only reading that fits the
            // bounds integer_bounds_for_type reports for `char`.
            std::int64_t byte = 0;
            if (i < expr.name.size()) byte = static_cast<std::int64_t>(static_cast<std::int8_t>(expr.name.at(i)));
            if (auto result = checked_assign_integer(elements[i], byte, loc); !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
        }
        return true;
    }

    // The evaluator's single "bind this expression to a place of this
    // declared type", the counterpart of codegen's
    // initialize_storage_from_expr. Five sites previously spelled out
    // `evaluate_expr_in_context` followed by `copy_into` by hand, so an
    // array destination -- which has neither of the two rules that reach
    // it ([dcl.init.aggr], [dcl.init.string]) expressible as "evaluate,
    // then copy a matching value" -- had no correct answer at any of
    // them.
    [[nodiscard]] std::expected<void, ConstexprError> initialize_cell_from_expr(const std::shared_ptr<Cell>& cell,
                                                                               const Type& type, const Expr& expr,
                                                                               const SourceLocation& loc) {
        auto string_result = try_initialize_array_cell_from_string_literal(cell, type, expr, loc);
        if (!string_result.has_value()) return std::unexpected(std::move(string_result).error());
        if (std::move(string_result).value()) return {};
        // [dcl.init]/17.5: an array destination reaches [dcl.init.aggr]
        // or [dcl.init.string] and nothing else, so anything that is
        // neither a braced list nor a string literal initializing an
        // array of `char` is ill-formed -- there is no array
        // copy-initialization to fall back on.
        if (type.kind == TypeKind::Array && type.element != nullptr && expr.kind != ExprKind::BracedInitList) {
            return std::unexpected(ConstexprError(loc, describe_invalid_array_initializer(type)));
        }
        // Codegen's mirror of this rule lives in
        // initialize_storage_from_expr; both are needed because the
        // evaluator never reaches codegen. `int w[6]{"hello"}` inside a
        // constexpr function bound `const char[6]` to the `int` element
        // `w[0]` and reported "constexpr assignment requires exactly
        // matching types" -- a message about the *store*, naming neither
        // operand, for what is really a destination [dcl.init.string]
        // does not apply to.
        if (type.kind == TypeKind::Named && is_scalar_type_name(type.name) && expr.kind != ExprKind::BracedInitList) {
            std::optional<Type> source_type = infer_unevaluated_expr_type(expr);
            if (source_type.has_value() && source_type->kind == TypeKind::Array) {
                return std::unexpected(ConstexprError(
                    loc, describe_cannot_initialize_from_array(type, *source_type,
                                                               expr.kind == ExprKind::StringLiteral)));
            }
        }
        auto value_result = evaluate_expr_in_context(expr, &type);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        return copy_into(cell, std::move(value_result).value(), loc);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> bind_read_only_span(const Type& span_type, const Expr& init_expr,
                                                            const SourceLocation& loc) {
        if (span_type.kind != TypeKind::Span || !span_type.pointee) {
            return std::unexpected(ConstexprError(loc, "malformed constexpr span type"));
        }
        if (span_type.is_mutable_ref) {
            return std::unexpected(ConstexprError(loc, "mutable std::span<T> is not supported during constant evaluation"));
        }

        auto result = std::make_shared<Cell>();
        result->type = span_type;
        SpanValue span{};

        if (init_expr.kind == ExprKind::StringLiteral) {
            auto pointer_cell_result = evaluate_expr(init_expr);
            if (!pointer_cell_result.has_value()) return std::unexpected(std::move(pointer_cell_result).error());
            std::shared_ptr<Cell> pointer_cell = std::move(pointer_cell_result).value();
            if (!pointer_cell->data.is_pointer() || !pointer_cell->data.pointer.storage ||
                !pointer_cell->data.pointer.storage->data.is_array()) {
                return std::unexpected(ConstexprError(loc, "string-literal span binding lost its backing storage"));
            }
            const PointerValue& pointer = pointer_cell->data.pointer;
            const ArrayValue& array = pointer.storage->data.array;
            if (!types_equal(*span_type.pointee, array.element_type)) {
                return std::unexpected(ConstexprError(loc, "string-literal element type does not match std::span element type"));
            }
            span.pointer = pointer;
            span.size = static_cast<std::int64_t>(array.elements.size()) - 1;
            result->data.set_span(std::move(span));
            return result;
        }

        auto source_result = resolve_lvalue(init_expr);
        if (!source_result.has_value()) return std::unexpected(std::move(source_result).error());
        LValue source = std::move(source_result).value();
        if (!source.cell->data.is_array()) {
            return std::unexpected(ConstexprError(loc, "std::span<const T> can only be constructed from an array or string literal"));
        }
        const ArrayValue& array = source.cell->data.array;
        if (!types_equal(*span_type.pointee, array.element_type)) {
            return std::unexpected(ConstexprError(loc, "array element type does not match std::span element type"));
        }
        span.pointer.storage = source.cell;
        span.pointer.index = 0;
        span.pointer.storage_id = "span#";
        span.pointer.storage_id += std::to_string(static_cast<std::int64_t>(string_storage_counter_ + 1));
        span.size = static_cast<std::int64_t>(array.elements.size());
        result->data.set_span(std::move(span));
        return result;
    }

    [[nodiscard]] std::expected<LValue, ConstexprError> resolve_lvalue(const Expr& expr) {
        if (auto result = tick(expr.loc, "resolving an lvalue"); !result.has_value()) return std::unexpected(std::move(result).error());
        switch (expr.kind) {
            case ExprKind::Identifier:
                return [&, this]() -> std::expected<LValue, ConstexprError> {
                    auto binding_result = lookup_binding(expr.name, expr.loc, expr.explicit_global_qualification);
                    if (!binding_result.has_value()) return std::unexpected(std::move(binding_result).error());
                    Binding binding = std::move(binding_result).value();
                    return LValue{binding.cell, binding.read_only};
                }();
            case ExprKind::Member:
                return [&, this]() -> std::expected<LValue, ConstexprError> {
                    auto base_result = resolve_lvalue(*expr.lhs);
                    if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                    LValue base = std::move(base_result).value();
                    if (!base.cell->data.is_object()) {
                        return std::unexpected(ConstexprError(expr.loc, "member access requires a constexpr object value"));
                    }
                    const ObjectValue& object = base.cell->data.object;
                    std::int64_t field_slot = object.field_index(expr.name);
                    if (field_slot < 0) {
                        std::string message{};
                        message += "unknown constexpr field '";
                        message += expr.name;
                        message += "'";
                        return std::unexpected(ConstexprError(expr.loc, message));
                    }
                    return LValue{object.fields[static_cast<std::size_t>(field_slot)].cell, base.read_only};
                }();
            case ExprKind::Subscript:
                return [&, this]() -> std::expected<LValue, ConstexprError> {
                    std::shared_ptr<Cell> base{};
                    bool base_read_only = false;
                    // Speculatively try lvalue resolution first (needed so
                    // that e.g. `arr[i] = 1` assigns through the real
                    // storage); an expression that isn't itself an lvalue
                    // (e.g. a function call returning an array by value)
                    // falls back to plain evaluation, matching the original
                    // try/catch(const ConstexprError&) fallback exactly.
                    auto base_lvalue_result = resolve_lvalue(*expr.lhs);
                    if (base_lvalue_result.has_value()) {
                        base = base_lvalue_result.value().cell;
                        base_read_only = base_lvalue_result.value().read_only;
                    } else {
                        auto base_value_result = evaluate_expr(*expr.lhs);
                        if (!base_value_result.has_value()) return std::unexpected(std::move(base_value_result).error());
                        base = std::move(base_value_result).value();
                    }
                    auto index_value_result = evaluate_expr(*expr.rhs);
                    if (!index_value_result.has_value()) return std::unexpected(std::move(index_value_result).error());
                    auto index_result = as_integer(index_value_result.value(), expr.loc);
                    if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                    std::int64_t index = index_result.value();
                    if (base->data.is_array()) {
                        const ArrayValue& array = base->data.array;
                        if (index < 0 || static_cast<std::size_t>(index) >= array.elements.size()) {
                            return std::unexpected(ConstexprError(expr.loc, "constexpr subscript out of bounds"));
                        }
                        return LValue{array.elements[static_cast<std::size_t>(index)], base_read_only};
                    }
                    if (base->data.is_span()) {
                        const SpanValue& span = base->data.span;
                        if (index < 0 || index >= span.size) {
                            return std::unexpected(ConstexprError(expr.loc, "constexpr span subscript out of bounds"));
                        }
                        PointerValue element_ptr = span.pointer;
                        element_ptr.index += index;
                        auto dereferenced = dereference_pointer(element_ptr, make_pointer_type_to(*base->type.pointee, false), expr.loc);
                        if (!dereferenced.has_value()) return std::unexpected(std::move(dereferenced).error());
                        return LValue{std::move(dereferenced).value(), true};
                    }
                    if (base->data.is_pointer()) {
                        if (!base->type.pointee) return std::unexpected(ConstexprError(expr.loc, "malformed constexpr pointer type"));
                        PointerValue shifted = base->data.pointer;
                        shifted.index += index;
                        if (!shifted.storage || !shifted.storage->data.is_array()) {
                            return std::unexpected(ConstexprError(expr.loc, "constexpr pointer does not point to indexable storage"));
                        }
                        if (shifted.index < 0 || static_cast<std::size_t>(shifted.index) >= shifted.storage->data.array.elements.size()) {
                            return std::unexpected(ConstexprError(expr.loc, "constexpr subscript out of bounds"));
                        }
                        auto dereferenced = dereference_pointer(shifted, base->type, expr.loc);
                        if (!dereferenced.has_value()) return std::unexpected(std::move(dereferenced).error());
                        return LValue{std::move(dereferenced).value(), true};
                    }
                    return std::unexpected(ConstexprError(expr.loc, "constexpr subscript requires an array, pointer, or std::span"));
                }();
            case ExprKind::Unary:
                if (expr.unary_op == UnaryOp::Deref) {
                    auto pointer_cell_result = evaluate_expr(*expr.lhs);
                    if (!pointer_cell_result.has_value()) return std::unexpected(std::move(pointer_cell_result).error());
                    std::shared_ptr<Cell> pointer_cell = std::move(pointer_cell_result).value();
                    if (!pointer_cell->data.is_pointer()) {
                        return std::unexpected(ConstexprError(expr.loc, "constexpr dereference requires a pointer"));
                    }
                    auto dereferenced = dereference_pointer(pointer_cell->data.pointer, pointer_cell->type, expr.loc);
                    if (!dereferenced.has_value()) return std::unexpected(std::move(dereferenced).error());
                    return LValue{std::move(dereferenced).value(), true};
                }
                break;
            default:
                break;
        }
        return std::unexpected(ConstexprError(expr.loc, "expression is not an assignable constexpr lvalue"));
    }

    [[nodiscard]] std::shared_ptr<Cell> make_string_literal_pointer(const Expr& expr) {
        Type array_type{};
        array_type.kind = TypeKind::Array;
        array_type.element = std::make_shared<Type>(named_type("char"));
        array_type.array_size = static_cast<std::int64_t>(expr.name.size()) + 1;
        auto storage = std::make_shared<Cell>();
        storage->type = array_type;
        ArrayValue array{};
        array.element_type = named_type("char");
        for (std::size_t i = 0; i < expr.name.size(); ++i) {
            // Index loop rather than a range-for, and std::int8_t rather
            // than the bare `signed char` shorthand: scpp's range-for
            // takes a fixed-size array, std::span or std::vector (not a
            // std::string), and requires `unsigned` be followed by
            // `int`/`long` (ch06 §6). The cast through std::int8_t
            // sign-extends bytes >= 0x80 to the negative `char` values
            // they name -- `char` is signed (ch06 §6), so a 0xC8 byte is
            // -56, which is also the only reading that fits the char
            // bounds integer_bounds_for_type reports.
            std::int64_t ch = static_cast<std::int64_t>(static_cast<std::int8_t>(expr.name.at(i)));
            array.elements.push_back(make_scalar_cell(named_type("char"), ch));
        }
        array.elements.push_back(make_scalar_cell(named_type("char"), 0));
        storage->data.set_array(std::move(array));

        auto result = std::make_shared<Cell>();
        result->type = make_const_char_pointer_type();
        PointerValue pointer{};
        pointer.storage = storage;
        pointer.storage_id = "string#";
        pointer.storage_id += std::to_string(static_cast<std::int64_t>(++string_storage_counter_));
        result->data.set_pointer(std::move(pointer));
        return result;
    }

    // Same progressive-outward walk `find_visible_global` performs for a
    // global, applied to the function index: an unqualified call is
    // sought in the enclosing namespace first and then outwards, and
    // `::f` skips the walk entirely. `functions_by_name_` remains, but it
    // now answers only "which overloads were registered under exactly
    // this name" -- the namespace question is answered here, once.
    //
    // The parser's own `qualify_same_namespace_function_calls` rewrite
    // reached some of this already, but only for calls appearing inside a
    // function *body* and only with the single full namespace prefix, so
    // a call in a global initializer, in an array bound, in an `alignas`
    // argument, or one naming a sibling of an *enclosing* namespace was
    // left unqualified and then failed here.
    [[nodiscard]] std::string describe_constexpr_candidate(const Function& fn, const std::string& display_name) {
        std::string result{};
        result += display_name;
        result += "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) result += ", ";
            result += describe_type_brief(fn.params[i].type);
        }
        result += ")";
        if (fn.is_generic_template) result += " [generic]";
        return result;
    }

    // [over.match.best]/2's diagnostic, in one place: a tie between free
    // functions, between methods and between constructors is the same
    // finding and must read the same way. The three sites used to have
    // one message between them.
    [[nodiscard]] std::string ambiguous_candidates_message(const std::string& what,
                                                           const std::vector<const Function*>& tied,
                                                           const std::string& display_name) {
        std::string message{};
        message += "ambiguous ";
        message += what;
        message += ": ";
        message += std::to_string(tied.size());
        message += " overloads match these argument types equally well and none is better than the others ([over.match.best])";
        for (const Function* fn : tied) {
            message += "\n  candidate: ";
            message += describe_constexpr_candidate(*fn, display_name);
        }
        return message;
    }

    [[nodiscard]] OptionalFunctionRef find_callable(const std::string& name, const std::vector<std::shared_ptr<Cell>>& args,
                                                bool explicit_global_qualification = false,
                                                const std::vector<ExprPtr>* arg_exprs = nullptr,
                                                std::vector<const Function*>* out_ambiguous = nullptr) {
        if (out_ambiguous != nullptr) out_ambiguous->clear();
        if (!explicit_global_qualification) {
            for (std::size_t depth = lookup_namespace_path_.size(); depth > 0; --depth) {
                std::string candidate{};
                for (std::size_t i = 0; i < depth; ++i) {
                    if (candidate.size() != 0) candidate += "::";
                    candidate += lookup_namespace_path_[i];
                }
                candidate += "::";
                candidate += name;
                if (OptionalFunctionRef found = find_callable_exact(candidate, args, arg_exprs, out_ambiguous);
                    found.has_value()) {
                    return found;
                }
                // A tie found at this depth is the answer: an outer scope
                // does not un-ambiguate an inner one.
                if (out_ambiguous != nullptr && out_ambiguous->size() > 1) return {};
            }
        }
        return find_callable_exact(name, args, arg_exprs, out_ambiguous);
    }

    [[nodiscard]] OptionalFunctionRef find_callable_exact(const std::string& name, const std::vector<std::shared_ptr<Cell>>& args,
                                                const std::vector<ExprPtr>* arg_exprs = nullptr,
                                                std::vector<const Function*>* out_ambiguous = nullptr) {
        if (!functions_by_name_.contains(name)) return {};
        // Collect every match rather than returning the first. Returning
        // the first made a constexpr call's meaning depend on the order
        // its overloads happened to be declared in -- the same defect
        // codegen's resolve_overload_by_type had, and leaving it here
        // would make `constexpr` a way to bypass the ambiguity check.
        std::vector<const Function*> matches{};
        for (std::size_t fn_index : functions_by_name_.at(name)) {
            const Function& fn = program_.functions[fn_index];
            if (fn.params.size() != args.size()) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const Expr* arg_expr = arg_exprs != nullptr && i < arg_exprs->size() ? (*arg_exprs)[i].get() : nullptr;
                if (arg_expr != nullptr && arg_expr->kind == ExprKind::BracedInitList) {
                    if (!braced_init_list_can_initialize(fn.params[i].type, arg_expr->args, arg_expr->loc)) {
                        params_match = false;
                        break;
                    }
                    continue;
                }
                if (!constexpr_argument_matches_parameter(fn.params[i].type, args[i])) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) matches.push_back(&fn);
        }
        return best_candidate(matches, args, arg_exprs, /*param_offset=*/0, out_ambiguous);
    }

    // [over.match.best], stated *once* for every resolver in this file.
    //
    // find_callable_exact used to be the only one that ranked; the method,
    // constructor and converting-constructor lookups each returned the
    // first candidate whose parameters matched, so a `constexpr` call
    // picked whichever overload happened to be declared first while
    // codegen -- which has always ranked -- picked the best one, and
    // neither reported the ambiguity C++ requires. Ranking is not a
    // property of *which kind* of function is being called, so it lives
    // in one place and every caller reaches it.
    //
    // `param_offset` is 1 for a member function, whose params[0] is the
    // implicit object parameter and has no argument opposite it.
    [[nodiscard]] OptionalFunctionRef best_candidate(std::vector<const Function*>& matches,
                                                     const std::vector<std::shared_ptr<Cell>>& args,
                                                     const std::vector<ExprPtr>* arg_exprs, std::size_t param_offset,
                                                     std::vector<const Function*>* out_ambiguous) {
        // [basic.def]/1: a declaration and the definition it belongs to
        // declare *one* function, so a redeclaration is not a second
        // candidate. This used to be expressed as "skip any function with
        // no body", which is a different rule and removed candidates the
        // language keeps: a `= delete`d function ([dcl.fct.def.delete]/2,
        // fixed once already) and -- the case that survived it -- an
        // *imported* non-constexpr function, whose body is not serialized
        // into the importing translation unit's .scppm at all. That made
        // a module-loaded candidate set differ from the identical
        // same-file one: `pick(int)` beside `constexpr pick(const int&)`
        // is ambiguous when both are written locally and silently folded
        // to the second when they are imported.
        //
        // 7.3(2.2) is the rule for a genuinely bodyless callee -- "a call
        // whose definition is unavailable for constant evaluation,
        // including an imported definition whose compile-time body is not
        // provided ... the program is ill-formed" -- and it is worded, as
        // every clause of 7.3 is, about what evaluation *would evaluate*.
        // It rejects after selection, in call_function; it does not
        // withdraw the declaration from overload resolution.
        std::vector<const Function*> distinct{};
        for (const Function* fn : matches) {
            bool redeclares_a_definition = false;
            if (!fn->body) {
                for (const Function* other : matches) {
                    if (other == fn || !other->body) continue;
                    if (other->member_owner_class != fn->member_owner_class) continue;
                    if (other->params.size() != fn->params.size()) continue;
                    bool same_signature = true;
                    for (std::size_t i = 0; i < fn->params.size(); ++i) {
                        if (!types_equal(other->params[i].type, fn->params[i].type)) {
                            same_signature = false;
                            break;
                        }
                    }
                    if (same_signature) {
                        redeclares_a_definition = true;
                        break;
                    }
                }
            }
            if (!redeclares_a_definition) distinct.push_back(fn);
        }
        matches = std::move(distinct);
        if (matches.empty()) return {};
        if (matches.size() == 1) return make_function_ref(*matches[0]);
        // [over.match.best]/2.4: a non-template is better than a template.
        std::vector<const Function*> non_generic{};
        for (const Function* fn : matches) {
            if (!fn->is_generic_template) non_generic.push_back(fn);
        }
        if (!non_generic.empty()) matches = std::move(non_generic);
        if (matches.size() == 1) return make_function_ref(*matches[0]);
        // [over.match.best] over [over.ics.rank], through the same shared
        // algebra codegen and movecheck use.
        //
        // Without it the evaluator stopped at the non-template
        // preference and called *every* remaining tie ambiguous, so a
        // `constexpr` call and a runtime call to the very same
        // expression selected differently: `f(a)` with `f(int&)` and
        // `f(const int&)` declared runs `f(int&)` at run time and was
        // rejected as ambiguous when folded.
        std::vector<std::vector<ArgumentConversion>> conversions{};
        for (const Function* fn : matches) {
            conversions.push_back(argument_conversions_for(*fn, args, arg_exprs, param_offset));
        }
        std::vector<std::size_t> best = best_viable_candidates(conversions);
        if (best.size() == 1) return make_function_ref(*matches[best[0]]);
        std::vector<const Function*> tied{};
        for (std::size_t index : best) tied.push_back(matches[index]);
        if (out_ambiguous != nullptr) *out_ambiguous = std::move(tied);
        return {};
    }

    // The evaluator's half of the shared [over.ics.rank] vocabulary.
    // Argument types come from the already-evaluated Cells, which is all
    // the evaluator has: a Cell records what an expression *is*, not the
    // value category the expression had, so /3.2.3's rvalue-reference
    // preference cannot fire here. Every other rule can, and those are
    // the ones that decide between two viable reference bindings.
    [[nodiscard]] std::vector<ArgumentConversion> argument_conversions_for(const Function& fn,
                                                                          const std::vector<std::shared_ptr<Cell>>& args,
                                                                          const std::vector<ExprPtr>* arg_exprs,
                                                                          std::size_t param_offset = 0) {
        auto strip_to_value = [](Type type) {
            if (type.kind == TypeKind::Reference && type.pointee != nullptr) type = *type.pointee;
            type.is_const_qualified = false;
            return type;
        };
        std::vector<ArgumentConversion> result{};
        for (std::size_t i = 0; i < args.size(); ++i) {
            ArgumentConversion conversion{};
            conversion.rank = ConversionRank::Identity;
            const Expr* arg_expr = arg_exprs != nullptr && i < arg_exprs->size() ? (*arg_exprs)[i].get() : nullptr;
            if (i + param_offset >= fn.params.size() || args[i] == nullptr ||
                (arg_expr != nullptr && arg_expr->kind == ExprKind::BracedInitList)) {
                // A braced list has no type of its own, so there is
                // nothing to rank; an unknown sequence compares equal to
                // every other one.
                conversion.unknown = true;
                result.push_back(conversion);
                continue;
            }
            const Type& param_type = fn.params[i + param_offset].type;
            Type target = param_type;
            if (param_type.kind == TypeKind::Reference) {
                conversion.binds_reference = true;
                conversion.reference_is_mutable = param_type.is_mutable_ref && !param_type.is_rvalue_ref;
                conversion.reference_is_rvalue = param_type.is_rvalue_ref;
                if (param_type.pointee != nullptr) target = *param_type.pointee;
            }
            Type from = decay_array_to_pointer(args[i]->type);
            if (types_equal(strip_to_value(from), strip_to_value(target))) {
                conversion.rank = ConversionRank::Identity;
            } else if (is_qualification_conversion(from, target)) {
                conversion.rank = ConversionRank::Qualification;
            } else {
                conversion.rank = ConversionRank::Conversion;
            }
            result.push_back(conversion);
        }
        return result;
    }

    [[nodiscard]] OptionalFunctionRef find_single_argument_converting_constructor(const std::string& class_name,
                                                                              const std::shared_ptr<Cell>& arg) {
        std::string constructor_name{};
        constructor_name += class_name;
        constructor_name += "_new";
        if (!functions_by_name_.contains(constructor_name)) return {};
        std::vector<std::shared_ptr<Cell>> args{};
        args.push_back(arg);
        std::vector<const Function*> matches{};
        for (std::size_t fn_index : functions_by_name_.at(constructor_name)) {
            const Function& fn = program_.functions[fn_index];
            if (fn.params.size() != 2) continue;
            const Type& param_type = fn.params[1].type;
            const Type& arg_type = arg->type;
            if (param_type.kind == TypeKind::Reference) {
                if (param_type.pointee && types_equal(*param_type.pointee, arg_type)) matches.push_back(&fn);
            } else if (types_equal_ignoring_top_level_const(param_type, arg_type)) {
                matches.push_back(&fn);
            }
        }
        return best_candidate(matches, args, /*arg_exprs=*/nullptr, /*param_offset=*/1, /*out_ambiguous=*/nullptr);
    }

    [[nodiscard]] bool is_same_or_base_class_type(const Type& expected, const Type& actual) const {
        if (types_equal(expected, actual)) return true;
        if (expected.kind != TypeKind::Named || actual.kind != TypeKind::Named) return false;
        if (!is_class_name(expected.name) || !is_class_name(actual.name)) return false;
        std::string current = actual.name;
        while (true) {
            if (!classes_by_name_.contains(current)) return false;
            const ClassDef& current_def = program_.classes[classes_by_name_.at(current)];
            auto base = current_def.direct_ordinary_base();
            if (!base.has_value()) return false;
            current = base->get().base_type.name;
            if (current == expected.name) return true;
        }
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> clone_cell_as_type(const std::shared_ptr<Cell>& cell, const Type& target_type,
                                                           const SourceLocation& loc) {
        auto clone = clone_cell(cell);
        if (!is_same_or_base_class_type(target_type, clone->type)) {
            return std::unexpected(ConstexprError(loc, "constexpr value is not compatible with requested parameter type"));
        }
        clone->type = target_type;
        if (clone->data.is_object()) clone->data.object.type_name = target_type.name;
        return clone;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> alias_cell_as_type(const std::shared_ptr<Cell>& cell, const Type& target_type,
                                                           const SourceLocation& loc) {
        if (!is_same_or_base_class_type(target_type, cell->type)) {
            return std::unexpected(ConstexprError(loc, "constexpr object is not compatible with requested reference type"));
        }
        if (!cell->data.is_object()) {
            return std::unexpected(ConstexprError(loc, "constexpr base-class binding requires an object value"));
        }
        const ObjectValue& object = cell->data.object;
        auto alias = std::make_shared<Cell>();
        alias->type = target_type;
        ObjectValue alias_object{};
        alias_object.type_name = target_type.name;
        for (const ObjectField& field : object.fields) alias_object.add_field(field.name, field.cell);
        alias->data.set_object(std::move(alias_object));
        return alias;
    }

    [[nodiscard]] bool constexpr_argument_matches_parameter(const Type& param_type, const std::shared_ptr<Cell>& arg) {
        const Type& arg_type = arg->type;
        if (param_type.kind == TypeKind::Reference) {
            if (!param_type.pointee) return false;
            if (types_equal(*param_type.pointee, arg_type) || is_same_or_base_class_type(*param_type.pointee, arg_type)) {
                return true;
            }
            if (param_type.pointee->is_const_qualified) {
                Type unqualified = *param_type.pointee;
                unqualified.is_const_qualified = false;
                return types_equal(unqualified, arg_type) || is_same_or_base_class_type(unqualified, arg_type);
            }
            return false;
        }
        // [conv.lval]/1 with [dcl.init]/16: a by-value parameter is
        // initialized from the argument's *value*, so the argument's own
        // top-level const takes no part in whether the candidate is
        // viable. Comparing it made a `constexpr int a = 1;` at namespace
        // scope -- whose cell carries the declared `const int` type --
        // fail to match an `int` parameter, so `f(a)` reported "no
        // constexpr/consteval overload matches" while codegen, which has
        // always compared through types_equal_ignoring_top_level_const,
        // accepted it. argument_conversions_for already strips it before
        // *ranking*; viability now asks the same question.
        Type param_value_type = param_type;
        param_value_type.is_const_qualified = false;
        Type arg_value_type = arg_type;
        arg_value_type.is_const_qualified = false;
        if (is_same_or_base_class_type(param_value_type, arg_value_type)) return true;
        if (param_type.kind == TypeKind::Named && is_record_name(param_type.name)) {
            return find_single_argument_converting_constructor(param_type.name, arg).has_value();
        }
        return false;
    }

    // Can this brace-enclosed initializer list initialize a parameter of
    // this type? A braced list has no type of its own, so it cannot be
    // pre-evaluated into a Cell and matched by type the way every other
    // argument is -- yet the question still has an exact answer, and
    // overload selection needs it: accepting on arity alone would let a
    // list silently bind to an overload it does not fit.
    //
    // The answer is obtained by *performing* the initialization against a
    // default cell of the parameter type and discarding the result, so it
    // is the same answer initialize_cell_from_brace_args will give when
    // the call is actually bound (call_with_expr_arg_views evaluates each
    // argument with its parameter type as the context type). Deriving it
    // from the real worker rather than from a separate predicate is what
    // keeps selection and binding from ever disagreeing.
    [[nodiscard]] bool braced_init_list_can_initialize(const Type& param_type, const std::vector<ExprPtr>& args,
                                                       const SourceLocation& loc) {
        if (param_type.kind == TypeKind::Reference) return false;
        auto cell_result = make_default_cell(param_type, loc);
        if (!cell_result.has_value()) return false;
        return initialize_cell_from_brace_args(std::move(cell_result).value(), param_type, args, loc).has_value();
    }

    [[nodiscard]] OptionalFunctionRef find_constructor(const std::string& class_name,
                                                   const std::vector<std::shared_ptr<Cell>>& args,
                                                   std::vector<const Function*>* out_ambiguous = nullptr) {
        if (out_ambiguous != nullptr) out_ambiguous->clear();
        std::string constructor_name{};
        constructor_name += class_name;
        constructor_name += "_new";
        if (!functions_by_name_.contains(constructor_name)) return {};
        std::vector<const Function*> matches{};
        for (std::size_t fn_index : functions_by_name_.at(constructor_name)) {
            const Function& fn = program_.functions[fn_index];
            if (fn.params.size() != args.size() + 1) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                // Hand-rolled reference/const matching here was a fifth
                // copy of constexpr_argument_matches_parameter that had
                // drifted: it knew nothing of base classes or of
                // converting constructors, so a constructor argument
                // accepted everywhere else was rejected here.
                if (!constexpr_argument_matches_parameter(fn.params[i + 1].type, args[i])) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) matches.push_back(&fn);
        }
        return best_candidate(matches, args, /*arg_exprs=*/nullptr, /*param_offset=*/1, out_ambiguous);
    }

    // The evaluator asks the same question as movecheck and codegen and now
    // gets it from the same place: the owner-anchored is_constructor_function
    // beside the AST. Its own copy tested only `name.ends_with("_new")`, so
    // execute_constructor_member_initializers below silently did nothing for
    // every monomorphized generic constructor.

    [[nodiscard]] std::expected<void, ConstexprError> apply_default_initializers_to_named_object(const std::shared_ptr<Cell>& object_cell, const Type& object_type,
                                                    const SourceLocation& loc) {
        if (object_type.kind != TypeKind::Named) return {};
        if (!object_cell->data.is_object()) return {};
        ObjectValue& object = object_cell->data.object;
        if (structs_by_name_.contains(object_type.name)) {
            const StructDef& struct_def = program_.structs[structs_by_name_.at(object_type.name)];
            for (const StructField& field : struct_def.fields) {
                if (!field.default_initializer) continue;
                std::int64_t field_slot = object.field_index(field.name);
                if (field_slot < 0) continue;
                if (auto result = apply_initializer_to_field(object.fields[static_cast<std::size_t>(field_slot)].cell,
                                                            field.type, *field.default_initializer, loc);
                    !result.has_value()) {
                    return result;
                }
            }
            return {};
        }
        if (classes_by_name_.contains(object_type.name)) {
            const ClassDef& class_def = program_.classes[classes_by_name_.at(object_type.name)];
            auto fields_result = collect_class_fields(class_def);
            if (!fields_result.has_value()) return std::unexpected(std::move(fields_result).error());
            for (const ClassField& field : fields_result.value()) {
                if (!field.default_initializer) continue;
                std::int64_t field_slot = object.field_index(field.name);
                if (field_slot < 0) continue;
                if (auto result = apply_initializer_to_field(object.fields[static_cast<std::size_t>(field_slot)].cell,
                                                            field.type, *field.default_initializer, loc);
                    !result.has_value()) {
                    return result;
                }
            }
        }
        return {};
    }

    // A sub-object that can absorb a whole run of initializers on its
    // own: an array, or a struct whose members a braced list may
    // initialize directly. Only such a sub-object can have had its
    // braces elided ([dcl.init.aggr]/15).
    [[nodiscard]] bool type_is_elidable_aggregate(const Type& type) const {
        if (type.kind == TypeKind::Array && type.element != nullptr) return true;
        return type.kind == TypeKind::Named && structs_by_name_.contains(type.name) && record_is_aggregate(type.name);
    }

    [[nodiscard]] bool aggregate_has_elidable_member(const Type& type) const {
        if (type.kind == TypeKind::Array && type.element != nullptr) {
            return type_is_elidable_aggregate(*type.element);
        }
        if (type.kind != TypeKind::Named || !structs_by_name_.contains(type.name)) return false;
        for (const StructField& field : program_.structs[structs_by_name_.at(type.name)].fields) {
            if (type_is_elidable_aggregate(field.type)) return true;
        }
        return false;
    }

    [[nodiscard]] std::expected<void, ConstexprError> report_leftover_cell_initializers(
        const Type& type, const std::vector<ExprPtr>& args, std::size_t index, const SourceLocation& loc) const {
        if (index >= args.size()) return {};
        std::string message{};
        message += "too many initializers for '";
        message += (type.name.empty() ? std::string{"array"} : type.name);
        message += "': every member is initialized and ";
        message += std::to_string(args.size() - index);
        message += (args.size() - index == 1 ? " initializer is" : " initializers are");
        message += " left over";
        return std::unexpected(ConstexprError(loc, message));
    }

    // One sub-object's worth of a brace-enclosed initializer list, taken
    // from `args` at `index`. This is the constant evaluator's copy of
    // the cursor model in Codegen::initialize_storage_from_brace_args_cursor
    // and must answer identically: a `constexpr` program and its runtime
    // twin are the same program, so if only one implementation elides
    // braces they disagree about what the program means.
    [[nodiscard]] std::expected<void, ConstexprError> initialize_cell_from_brace_args_cursor(
        const std::shared_ptr<Cell>& cell, const Type& type, const std::vector<ExprPtr>& args, std::size_t& index,
        const SourceLocation& loc) {
        if (index >= args.size()) return {};
        const Expr& next = *args[index];
        if (next.kind == ExprKind::BracedInitList) {
            ++index;
            return initialize_cell_from_brace_args(cell, type, next.args, loc);
        }
        // [dcl.init.aggr]/4.2 -> [dcl.init.string]: a string literal
        // initializing a sub-object of array-of-`char` type initializes
        // the whole sub-object and braces are not elided into it. Codegen's
        // cursor asks this in the same position and for the same reason.
        if (string_literal_initializes_char_array(type, next)) {
            ++index;
            auto string_result = try_initialize_array_cell_from_string_literal(cell, type, next, loc);
            if (!string_result.has_value()) return std::unexpected(std::move(string_result).error());
            return {};
        }
        if (!type_is_elidable_aggregate(type)) {
            ++index;
            return initialize_cell_from_expr(cell, type, next, loc);
        }
        if (type.kind == TypeKind::Named) {
            std::optional<Type> source_type = infer_unevaluated_expr_type(next);
            if (source_type.has_value() && types_equal(*source_type, type)) {
                ++index;
                return initialize_cell_from_expr(cell, type, next, loc);
            }
            // Reached only when the braces were elided, so this record
            // never passed through initialize_cell_from_brace_args and
            // its default member initializers have not run yet.
            if (auto result = apply_default_initializers_to_named_object(cell, type, loc); !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
        }
        std::int64_t covered = 0;
        return fill_aggregate_cell_from_cursor(cell, type, args, index, covered, loc);
    }

    // Walks an aggregate's sub-objects in declaration order, giving each
    // one the cursor and stopping when the run is exhausted. `covered`
    // reports how many sub-objects an initializer reached; sub-objects
    // past it keep the value make_default_cell and the default member
    // initializers gave them.
    [[nodiscard]] std::expected<void, ConstexprError> fill_aggregate_cell_from_cursor(
        const std::shared_ptr<Cell>& cell, const Type& type, const std::vector<ExprPtr>& args, std::size_t& index,
        std::int64_t& covered, const SourceLocation& loc) {
        covered = 0;
        if (type.kind == TypeKind::Array && type.element != nullptr) {
            if (!cell->data.is_array()) {
                return std::unexpected(ConstexprError(loc, "internal error: expected array storage for an array initializer"));
            }
            std::vector<std::shared_ptr<Cell>>& elements = cell->data.array.elements;
            while (static_cast<std::size_t>(covered) < elements.size() && index < args.size()) {
                if (auto result = initialize_cell_from_brace_args_cursor(elements[static_cast<std::size_t>(covered)],
                                                                        *type.element, args, index, loc);
                    !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                ++covered;
            }
            return {};
        }
        if (type.kind != TypeKind::Named || !structs_by_name_.contains(type.name)) {
            return std::unexpected(ConstexprError(loc, "internal error: expected an aggregate type for a brace-enclosed initializer list"));
        }
        if (!cell->data.is_object()) {
            return std::unexpected(ConstexprError(loc, "internal error: expected object storage for a record initializer"));
        }
        const StructDef& struct_def = program_.structs[structs_by_name_.at(type.name)];
        ObjectValue& object = cell->data.object;
        while (static_cast<std::size_t>(covered) < struct_def.fields.size() && index < args.size()) {
            const StructField& field = struct_def.fields[static_cast<std::size_t>(covered)];
            std::int64_t field_slot = object.field_index(field.name);
            if (field_slot < 0) {
                return std::unexpected(ConstexprError(loc, "internal error: missing member storage for '" + field.name + "'"));
            }
            if (auto result = initialize_cell_from_brace_args_cursor(
                    object.fields[static_cast<std::size_t>(field_slot)].cell, field.type, args, index, loc);
                !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
            ++covered;
        }
        return {};
    }

    // `int values[3]{1, 2, 3};` during constant evaluation. Spec §9.4(1)
    // adopts [dcl.array] unchanged, so this is [dcl.init.aggr]: element
    // i takes initializer i, and any element the list does not reach
    // keeps the value-initialization make_default_cell already gave it.
    // Both places a braced list can meet an array -- a local declaration
    // and a member/field initializer -- call this, because the evaluator
    // previously answered the question twice and identically wrongly:
    // each assumed a braced list meant a *constructor call*, built
    // `type.name + "_new"`, and an array type's name is empty, so even
    // `int a[3]{}` failed with "no constexpr/consteval constructor
    // matches for type ''".
    [[nodiscard]] std::expected<void, ConstexprError> initialize_array_cell_from_brace_args(
        const std::shared_ptr<Cell>& cell, const Type& array_type, const std::vector<ExprPtr>& args,
        const SourceLocation& loc) {
        if (!array_type.element) return std::unexpected(ConstexprError(loc, "malformed array type in constexpr evaluator"));
        if (!cell->data.is_array()) {
            return std::unexpected(ConstexprError(loc, "internal error: expected array storage for an array initializer"));
        }
        // [dcl.init.aggr]/4.2: `char w[6] = {"hello"}` is the same
        // [dcl.init.string] initialization `char w[6] = "hello"` is, not
        // a six-element list whose first initializer is a string. Asked
        // before the element walk, exactly as codegen's array branch does.
        if (args.size() == 1 && args[0] != nullptr) {
            auto string_result = try_initialize_array_cell_from_string_literal(cell, array_type, *args[0], loc);
            if (!string_result.has_value()) return std::unexpected(std::move(string_result).error());
            if (std::move(string_result).value()) return {};
        }
        std::vector<std::shared_ptr<Cell>>& elements = cell->data.array.elements;
        // The element count answers "too many initializers?" directly
        // only when no element can absorb more than one of them. Once an
        // element is itself an aggregate its braces may be elided
        // ([dcl.init.aggr]/15), so the run is longer than the array and
        // the leftover check below is what reports an overlong list.
        if (!aggregate_has_elidable_member(array_type) && args.size() > elements.size()) {
            std::string message{};
            message += "too many initializers for array of ";
            message += std::to_string(elements.size());
            message += (elements.size() == 1 ? " element: " : " elements: ");
            message += std::to_string(args.size());
            message += " given";
            return std::unexpected(ConstexprError(loc, message));
        }
        std::size_t index = 0;
        std::int64_t covered = 0;
        if (auto result = fill_aggregate_cell_from_cursor(cell, array_type, args, index, covered, loc);
            !result.has_value()) {
            return std::unexpected(std::move(result).error());
        }
        return report_leftover_cell_initializers(array_type, args, index, loc);
    }

    // `S s{1, 2};` during constant evaluation -- [dcl.init.aggr] for a
    // record, the struct sibling of initialize_array_cell_from_brace_args
    // above. The evaluator is a second implementation of this rule and
    // failed differently from codegen: it assumed a braced list always
    // meant a *constructor call*, so a struct with no constructor was
    // rejected as "no constexpr/consteval constructor matches" however
    // well-formed the list was.
    [[nodiscard]] std::expected<void, ConstexprError> initialize_record_cell_from_brace_args(
        const std::shared_ptr<Cell>& cell, const StructDef& struct_def, const std::vector<ExprPtr>& args,
        const SourceLocation& loc) {
        if (!cell->data.is_object()) {
            return std::unexpected(ConstexprError(loc, "internal error: expected object storage for a record initializer"));
        }
        Type record_type = named_type(struct_def.name);
        // See the array sibling: the member count is the right answer
        // only when braces cannot have been elided inside this record.
        if (!aggregate_has_elidable_member(record_type) && args.size() > struct_def.fields.size()) {
            std::string message{};
            message += "too many initializers for '";
            message += struct_def.name;
            message += "': ";
            message += std::to_string(struct_def.fields.size());
            message += (struct_def.fields.size() == 1 ? " member, " : " members, ");
            message += std::to_string(args.size());
            message += " given";
            return std::unexpected(ConstexprError(loc, message));
        }
        std::size_t index = 0;
        std::int64_t covered = 0;
        if (auto result = fill_aggregate_cell_from_cursor(cell, record_type, args, index, covered, loc);
            !result.has_value()) {
            return std::unexpected(std::move(result).error());
        }
        return report_leftover_cell_initializers(record_type, args, index, loc);
    }

    // True when any element of a braced list is itself a braced list.
    // Such an element has no type of its own, so it cannot take part in
    // constructor overload resolution -- the enclosing list can only be
    // aggregate initialization, and the paths that would otherwise
    // pre-evaluate their arguments to select a constructor consult this
    // first.
    [[nodiscard]] static bool args_contain_braced_init_list(const std::vector<ExprPtr>& args) {
        for (const ExprPtr& arg : args) {
            if (arg != nullptr && arg->kind == ExprKind::BracedInitList) return true;
        }
        return false;
    }

    // Initializes a cell of an arbitrary type from a braced list,
    // dispatching to the array or record worker. This is the evaluator's
    // counterpart to codegen's initialize_storage_from_brace_args, and
    // exists so a *nested* list has exactly one place to be interpreted
    // however deep it sits and whichever shape encloses it.
    [[nodiscard]] std::expected<void, ConstexprError> initialize_cell_from_brace_args(
        const std::shared_ptr<Cell>& cell, const Type& type, const std::vector<ExprPtr>& args,
        const SourceLocation& loc) {
        if (type.kind == TypeKind::Array) {
            return initialize_array_cell_from_brace_args(cell, type, args, loc);
        }
        if (type.kind == TypeKind::Named && record_is_aggregate(type.name)) {
            if (auto result = apply_default_initializers_to_named_object(cell, type, loc); !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
            return initialize_record_cell_from_brace_args(cell, program_.structs[structs_by_name_.at(type.name)], args,
                                                          loc);
        }
        if (args.empty()) return {};
        std::string message{};
        message += "a brace-enclosed initializer list cannot initialize '";
        message += (type.name.empty() ? std::string{"this type"} : type.name);
        message += "': only an array or an aggregate struct ([dcl.init.aggr]) takes its members from a list";
        return std::unexpected(ConstexprError(loc, message));
    }

    [[nodiscard]] std::expected<void, ConstexprError> apply_initializer_to_field(std::shared_ptr<Cell>& field_cell, const Type& field_type, const Initializer& init,
                                    const SourceLocation& loc) {
        if (field_type.kind == TypeKind::Reference) {
            OptionalExprRef ref_expr = optional_expr_ref(init.expr);
            if (init.has_brace_args) {
                if (init.brace_args.size() != 1) {
                    return std::unexpected(ConstexprError(loc, "a reference member must be initialized with exactly one expression"));
                }
                ref_expr = optional_expr_ref(init.brace_args[0]);
            }
            if (!ref_expr.has_value()) return std::unexpected(ConstexprError(loc, "a reference member must be initialized"));
            const Expr& ref_init = ref_expr->get();
            if (field_type.is_mutable_ref) {
                auto lvalue_result = resolve_lvalue(ref_init);
                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                field_cell = lvalue_result.value().cell;
            } else {
                // Speculative: prefer binding through the referenced
                // lvalue's real storage; a non-lvalue initializer (e.g. a
                // temporary) falls back to plain evaluation, exactly like
                // the original try/catch(const ConstexprError&) fallback.
                auto lvalue_result = resolve_lvalue(ref_init);
                if (lvalue_result.has_value()) {
                    field_cell = lvalue_result.value().cell;
                } else {
                    auto value_result = evaluate_expr(ref_init);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    field_cell = std::move(value_result).value();
                }
            }
            return {};
        }
        if (field_type.kind == TypeKind::Span) {
            OptionalExprRef span_expr = optional_expr_ref(init.expr);
            if (init.has_brace_args) {
                if (init.brace_args.size() != 1) {
                    return std::unexpected(ConstexprError(loc, "a span member must be initialized with exactly one array expression"));
                }
                span_expr = optional_expr_ref(init.brace_args[0]);
            }
            if (!span_expr.has_value()) return std::unexpected(ConstexprError(loc, "a span member must be initialized"));
            auto span_result = bind_read_only_span(field_type, span_expr->get(), loc);
            if (!span_result.has_value()) return std::unexpected(std::move(span_result).error());
            field_cell = std::move(span_result).value();
            return {};
        }
        if (field_type.kind == TypeKind::Array && init.has_brace_args) {
            return initialize_array_cell_from_brace_args(field_cell, field_type, init.brace_args, loc);
        }
        if (field_type.kind == TypeKind::Named &&
            (is_class_name(field_type.name) || structs_by_name_.contains(field_type.name)) && init.has_brace_args) {
            // See the VarDecl path's note: a braced-list element has no
            // type to resolve a constructor with.
            if (args_contain_braced_init_list(init.brace_args) && record_is_aggregate(field_type.name)) {
                return initialize_cell_from_brace_args(field_cell, field_type, init.brace_args, loc);
            }
            std::vector<std::shared_ptr<Cell>> arg_values{};
            arg_values.reserve(init.brace_args.size());
            for (const ExprPtr& arg : init.brace_args) {
                auto arg_result = evaluate_expr(*arg);
                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                arg_values.push_back(std::move(arg_result).value());
            }
            if (OptionalFunctionRef ctor_ref = find_constructor(field_type.name, arg_values);
                ctor_ref.has_value()) {
                const Function& ctor = ctor_ref->get();
                std::vector<Binding> bindings{};
                bindings.reserve(ctor.params.size());
                bindings.push_back(Binding{field_cell, false});
                for (std::size_t i = 1; i < ctor.params.size(); ++i) {
                    const Param& param = ctor.params[i];
                    const Expr& arg_expr = *init.brace_args[i - 1];
                    if (param.type.kind == TypeKind::Reference) {
                        if (param.type.is_rvalue_ref) {
                            auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            bindings.push_back(Binding{std::move(value_result).value(), false});
                        } else if (param.type.is_mutable_ref) {
                            auto lvalue_result = resolve_lvalue(arg_expr);
                            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                            bindings.push_back(Binding{lvalue_result.value().cell, false});
                        } else {
                            auto lvalue_result = resolve_lvalue(arg_expr);
                            if (lvalue_result.has_value()) {
                                bindings.push_back(Binding{lvalue_result.value().cell, true});
                            } else {
                                auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                                bindings.push_back(Binding{std::move(value_result).value(), true});
                            }
                        }
                    } else {
                        auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        bindings.push_back(Binding{std::move(value_result).value(), false});
                    }
                }
                auto call_result = call_function(ctor, std::move(bindings), loc);
                if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
                return {};
            }
            if (init.brace_args.empty()) {
                return apply_default_initializers_to_named_object(field_cell, field_type, loc);
            }
            // [dcl.init.aggr] for a member whose own initializer is a
            // braced list -- `struct Out { In i{4, 5}; };`. This is the
            // evaluator's *third* constructor-call site, and #484 gave
            // the aggregate branch only to the other two, so a
            // record-typed member with a multi-element default member
            // initializer still failed here with "requires exactly one
            // expression" even with no nesting involved.
            if (record_is_aggregate(field_type.name)) {
                return initialize_cell_from_brace_args(field_cell, field_type, init.brace_args, loc);
            }
        }
        if (init.has_brace_args) {
            if (init.brace_args.empty()) return {};
            if (init.brace_args.size() != 1) {
                return std::unexpected(ConstexprError(loc, "brace-initialization of this member requires exactly one expression"));
            }
            return initialize_cell_from_expr(field_cell, field_type, *init.brace_args[0], loc);
        }
        if (init.expr) {
            return initialize_cell_from_expr(field_cell, field_type, *init.expr, loc);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> execute_constructor_member_initializers(const Function& fn) {
        if (!is_constructor_function(fn)) return {};
        auto this_binding_result = lookup_binding("this", fn.loc);
        if (!this_binding_result.has_value()) return std::unexpected(std::move(this_binding_result).error());
        Binding this_binding = std::move(this_binding_result).value();
        if (!this_binding.cell->data.is_object()) {
            return std::unexpected(ConstexprError(fn.loc, "constructor receiver is not an object during constant evaluation"));
        }
        ObjectValue& object = this_binding.cell->data.object;
        if (structs_by_name_.contains(fn.member_owner_class)) {
            const StructDef& struct_def = program_.structs[structs_by_name_.at(fn.member_owner_class)];
            for (const StructField& field : struct_def.fields) {
                OptionalInitializerRef selected{};
                for (const MemberInitializer& init : fn.member_initializers) {
                    if (init.member_name == field.name) {
                        selected = make_initializer_ref(init.initializer);
                        break;
                    }
                }
                if (!selected.has_value() && field.default_initializer) {
                    selected = make_initializer_ref(*field.default_initializer);
                }
                if (!selected.has_value()) continue;
                std::int64_t field_slot = object.field_index(field.name);
                if (field_slot < 0) {
                    std::string message{};
                    message += "missing constexpr storage for field '";
                    message += field.name;
                    message += "'";
                    return std::unexpected(ConstexprError(fn.loc, message));
                }
                if (auto result = apply_initializer_to_field(object.fields[static_cast<std::size_t>(field_slot)].cell,
                                                            field.type, selected->get(), fn.loc);
                    !result.has_value()) {
                    return result;
                }
            }
            return {};
        }
        if (!classes_by_name_.contains(fn.member_owner_class)) {
            std::string message{};
            message += "missing constexpr class definition for '";
            message += fn.member_owner_class;
            message += "'";
            return std::unexpected(ConstexprError(fn.loc, message));
        }
        const ClassDef& owner_def = program_.classes[classes_by_name_.at(fn.member_owner_class)];
        for (const ClassField& field : owner_def.fields) {
            OptionalInitializerRef selected{};
            for (const MemberInitializer& init : fn.member_initializers) {
                if (init.member_name == field.name) {
                    selected = make_initializer_ref(init.initializer);
                    break;
                }
            }
            if (!selected.has_value() && field.default_initializer) {
                selected = make_initializer_ref(*field.default_initializer);
            }
            if (!selected.has_value()) continue;
            std::int64_t field_slot = object.field_index(field.name);
            if (field_slot < 0) {
                std::string message{};
                message += "missing constexpr storage for field '";
                message += field.name;
                message += "'";
                return std::unexpected(ConstexprError(fn.loc, message));
            }
            if (auto result = apply_initializer_to_field(object.fields[static_cast<std::size_t>(field_slot)].cell,
                                                         field.type, selected->get(), fn.loc);
                !result.has_value()) {
                return result;
            }
        }
        return {};
    }

    [[nodiscard]] bool is_class_name(const std::string& name) const {
        return classes_by_name_.contains(name);
    }

    [[nodiscard]] bool is_record_name(const std::string& name) const {
        return classes_by_name_.contains(name) || structs_by_name_.contains(name);
    }

    // [dcl.init.aggr], stated once for the evaluator's two
    // constructor-call sites (evaluate_constructor_expr and the
    // local-declaration path) so they cannot drift apart. The codegen
    // side answers the same question in Codegen::record_is_aggregate;
    // the two tables differ but the rule does not.
    [[nodiscard]] bool record_is_aggregate(const std::string& name) const {
        if (classes_by_name_.contains(name)) return false;
        if (!structs_by_name_.contains(name)) return false;
        for (const StructField& field : program_.structs[structs_by_name_.at(name)].fields) {
            if (field.access != AccessSpecifier::Public) return false;
        }
        return !functions_by_name_.contains(name + "_new");
    }

    // ch11 §11.5(1) requires every class to *declare* an explicit virtual
    // destructor, while ch07 §7.2(2.4)/§7.3(4) forbid required constant
    // evaluation from executing a *user-defined* one. Those are different
    // terms: `virtual ~T() = default;` is user-declared and not
    // user-defined, so it satisfies both and stays evaluable. Hence the
    // body test -- a destructor slot with no body is a defaulted one and
    // runs no user code.
    [[nodiscard]] bool has_user_defined_destructor(const std::string& record_name) const {
        std::string destructor_name{};
        destructor_name += record_name;
        destructor_name += "_delete";
        if (!functions_by_name_.contains(destructor_name)) return false;
        for (std::size_t fn_index : functions_by_name_.at(destructor_name)) {
            if (program_.functions[fn_index].body) return true;
        }
        return false;
    }

    // ch07 §7.3(4) makes required constant evaluation ill-formed when it
    // would "execute any operation that requires a user-defined
    // destructor", and ch07 §7.2(1.4) admits only a *trivial* struct type.
    // Both ask about the objects an evaluation would destroy, not about
    // the spelling of one name: destroying a `W` also destroys every base
    // subobject, member and array element it owns, running each of their
    // destructors.
    //
    // This used to ask "is this named type a *class* that declares a
    // body-having destructor", which is a list-shaped approximation of
    // that question and silently admitted three families: a `struct` of
    // any shape (the guard tested `classes_by_name_` only, though the
    // `_delete` mangling is common to both records), a record reached as
    // a member of the declared type, and one reached as an array element
    // -- a guard keyed on `type.kind == Named` cannot express the array
    // case at all. In each the destructor was neither diagnosed nor run,
    // so a destructor body calling a non-constexpr function compiled
    // clean.
    //
    // Pointer, Reference and Span deliberately terminate the walk:
    // destroying a pointer or a non-owning view destroys no pointee (and
    // ch07 §7.3(3) forbids `delete` outright), so descending through them
    // would reject programs the spec permits. `visiting` breaks cycles,
    // which those same indirections make representable.
    [[nodiscard]] std::optional<std::string> type_owning_user_defined_destructor(const Type& type,
                                                                                std::vector<std::string>& visiting) const {
        if (type.kind == TypeKind::Array) {
            if (!type.element) return std::nullopt;
            return type_owning_user_defined_destructor(*type.element, visiting);
        }
        if (type.kind != TypeKind::Named || !is_record_name(type.name)) return std::nullopt;
        for (const std::string& active : visiting) {
            if (active == type.name) return std::nullopt;
        }
        if (has_user_defined_destructor(type.name)) return type.name;
        visiting.push_back(type.name);
        std::optional<std::string> found{};
        if (structs_by_name_.contains(type.name)) {
            const StructDef& struct_def = program_.structs[structs_by_name_.at(type.name)];
            for (const StructField& field : struct_def.fields) {
                found = type_owning_user_defined_destructor(field.type, visiting);
                if (found.has_value()) break;
            }
        } else {
            const ClassDef& class_def = program_.classes[classes_by_name_.at(type.name)];
            if (auto base = class_def.direct_ordinary_base(); base.has_value()) {
                found = type_owning_user_defined_destructor(base->get().base_type, visiting);
            }
            if (!found.has_value()) {
                for (const ClassField& field : class_def.fields) {
                    found = type_owning_user_defined_destructor(field.type, visiting);
                    if (found.has_value()) break;
                }
            }
        }
        visiting.pop_back();
        return found;
    }

    [[nodiscard]] std::expected<void, ConstexprError> reject_user_defined_destructor_execution(const Type& type, const SourceLocation& loc) const {
        std::vector<std::string> visiting{};
        std::optional<std::string> owner = type_owning_user_defined_destructor(type, visiting);
        if (!owner.has_value()) return {};
        std::string message{};
        message += "required constant evaluation cannot execute user-defined destructor of '";
        message += *owner;
        message += "'";
        const Type* declared = &type;
        while (declared->kind == TypeKind::Array && declared->element) declared = declared->element.get();
        if (declared->kind == TypeKind::Named && declared->name != *owner) {
            message += ", reached from '";
            message += declared->name;
            message += "'";
        }
        return std::unexpected(ConstexprError(loc, message));
    }

    // `static_cast<T>(v)` for any of ch06 §6's twenty scalar types. This
    // used to enumerate four of them -- `double`, `bool`, `int`, `char`
    // -- and reject the rest with a message naming a "Phase D1" that is
    // defined nowhere. Every branch now asks `scpp.ast`'s scalar model
    // what T is, so adding a scalar type cannot leave this behind.
    //
    // The integral path deliberately does not route through `double` the
    // way the old `int`/`char` branch did. That was lossless only
    // because the two types it served are narrower than double's 53-bit
    // mantissa; `static_cast<int64_t>` of a large integer through a
    // double would silently round. An integral source is converted as an
    // integer and only a floating source truncates toward zero.
    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> cast_value(const Type& target_type, const std::shared_ptr<Cell>& operand,
                                                   const SourceLocation& loc) {
        if (is_floating_like(target_type)) {
            auto result = as_double(operand, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return make_float_cell_as(target_type, result.value());
        }
        if (is_named_type(target_type, "bool")) {
            auto result = as_bool(operand, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return make_bool_cell(result.value());
        }
        if (is_integer_like(target_type)) {
            auto result = std::make_shared<Cell>();
            result->type = target_type;
            if (is_floating_like(operand->type)) {
                // [conv.fpint]/1: the fractional part is discarded, and a
                // truncated value the destination cannot represent is
                // undefined -- which in a constant expression is
                // ill-formed, not a wrapped value.
                auto double_result = as_double(operand, loc);
                if (!double_result.has_value()) return std::unexpected(std::move(double_result).error());
                if (!float_value_is_integral_representable(double_result.value())) {
                    return std::unexpected(ConstexprError(loc, "constexpr integer overflow"));
                }
                std::int64_t truncated = static_cast<std::int64_t>(double_result.value());
                if (auto assign_result = checked_assign_integer(result, truncated, loc); !assign_result.has_value()) {
                    return std::unexpected(std::move(assign_result).error());
                }
                return result;
            }
            auto integer_result = as_integer(operand, loc);
            if (!integer_result.has_value()) return std::unexpected(std::move(integer_result).error());
            // [conv.integral]/2-3: an integer conversion never overflows;
            // the result is the value congruent modulo 2**N. This is the
            // one place a scalar cast differs from an assignment, and
            // running it through checked_assign_integer -- which enforces
            // the *assignment* rule -- is what made the constant
            // evaluator reject `static_cast<char>(200)` as an overflow
            // while codegen produced -56 for the same expression.
            result->data.set_integer(
                scalar_converted_integer_value(integer_result.value(), std::string_view{target_type.name}, host_pointer_bit_width()));
            return result;
        }
        // ch14 §14.1(1)-(2): an enumeration target holds the value of its
        // underlying type, reached the same way a scalar target is.
        if (is_enum_like(target_type)) {
            auto integer_result = as_integer(operand, loc);
            if (!integer_result.has_value()) return std::unexpected(std::move(integer_result).error());
            auto result = std::make_shared<Cell>();
            result->type = target_type;
            result->data.set_integer(integer_result.value());
            return result;
        }
        return std::unexpected(ConstexprError(loc, "constant evaluation of a cast supports scalar and enumeration target types only"));
    }

    // A double outside int64_t's range (or a NaN) has no int64_t value
    // to check bounds against -- `static_cast<std::int64_t>` of it is
    // undefined behaviour in the host compiler, so the range test has to
    // happen before the conversion, not after. Such a value cannot be in
    // range for any scalar type, so reporting it as an overflow is the
    // same answer the bounds check would give.
    [[nodiscard]] bool float_value_is_integral_representable(double value) const {
        if (!(value == value)) return false;
        return value >= -9223372036854775808.0 && value < 9223372036854775808.0;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_binary_numeric(const Expr& expr, const std::shared_ptr<Cell>& lhs,
                                                                const std::shared_ptr<Cell>& rhs) {
        if (is_floating_like(lhs->type) || is_floating_like(rhs->type)) {
            auto left_result = as_double(lhs, expr.loc);
            if (!left_result.has_value()) return std::unexpected(std::move(left_result).error());
            auto right_result = as_double(rhs, expr.loc);
            if (!right_result.has_value()) return std::unexpected(std::move(right_result).error());
            double left = left_result.value();
            double right = right_result.value();
            // Mirrors the integral arm below: two operands of the same
            // type produce that type, so `float32_t + float32_t` stays a
            // float32_t rather than silently becoming a double that no
            // float32_t place will then accept.
            Type float_result_type = types_equal(lhs->type, rhs->type) ? lhs->type : named_type("double");
            switch (expr.binary_op) {
                case BinaryOp::Add: return make_float_cell_as(float_result_type, left + right);
                case BinaryOp::Sub: return make_float_cell_as(float_result_type, left - right);
                case BinaryOp::Mul: return make_float_cell_as(float_result_type, left * right);
                case BinaryOp::Div: return make_float_cell_as(float_result_type, left / right);
                case BinaryOp::Eq: return make_bool_cell(left == right);
                case BinaryOp::Ne: return make_bool_cell(left != right);
                case BinaryOp::Lt: return make_bool_cell(left < right);
                case BinaryOp::Gt: return make_bool_cell(left > right);
                case BinaryOp::Le: return make_bool_cell(left <= right);
                case BinaryOp::Ge: return make_bool_cell(left >= right);
                default: break;
            }
        } else {
            auto left_result = as_integer(lhs, expr.loc);
            if (!left_result.has_value()) return std::unexpected(std::move(left_result).error());
            auto right_result = as_integer(rhs, expr.loc);
            if (!right_result.has_value()) return std::unexpected(std::move(right_result).error());
            std::int64_t left = left_result.value();
            std::int64_t right = right_result.value();
            Type result_type = types_equal(lhs->type, rhs->type) ? lhs->type : named_type("int");
            switch (expr.binary_op) {
                case BinaryOp::Add: {
                    std::int64_t result{};
                    if (__builtin_add_overflow(left, right, &result)) return std::unexpected(ConstexprError(expr.loc, "constexpr integer overflow"));
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Sub: {
                    std::int64_t result{};
                    if (__builtin_sub_overflow(left, right, &result)) return std::unexpected(ConstexprError(expr.loc, "constexpr integer overflow"));
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Mul: {
                    std::int64_t result{};
                    if (__builtin_mul_overflow(left, right, &result)) return std::unexpected(ConstexprError(expr.loc, "constexpr integer overflow"));
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Div:
                    if (right == 0) return std::unexpected(ConstexprError(expr.loc, "constexpr division by zero"));
                    return make_checked_int_cell_as(result_type, left / right, expr.loc);
                case BinaryOp::Eq: return make_bool_cell(left == right);
                case BinaryOp::Ne: return make_bool_cell(left != right);
                case BinaryOp::Lt: return make_bool_cell(left < right);
                case BinaryOp::Gt: return make_bool_cell(left > right);
                case BinaryOp::Le: return make_bool_cell(left <= right);
                case BinaryOp::Ge: return make_bool_cell(left >= right);
                default: break;
            }
        }
        return std::unexpected(ConstexprError(expr.loc, "unsupported constexpr binary operator"));
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> call_function(const Function& fn, std::vector<Binding> bindings,
                                                      const SourceLocation& loc) {
        if (auto result = tick(loc, "calling an immediate function"); !result.has_value()) return std::unexpected(std::move(result).error());
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly)
            return [&]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                return std::unexpected(ConstexprError(loc, "immediate evaluation may only call constexpr/consteval functions"));
            }();
        // [dcl.fct.def.delete]/2, asked at the evaluator's single call
        // funnel -- plain call, method call and constructor all arrive
        // here -- so a constant-evaluated call and a runtime call give
        // the same answer to the same question, in the same words.
        if (fn.is_deleted) {
            std::string subject{"'"};
            subject += fn.name;
            subject += "'";
            return std::unexpected(ConstexprError(loc, deleted_function_error_message(subject, fn.loc)));
        }
        if (!fn.body) return std::unexpected(ConstexprError(loc, "cannot evaluate a declaration-only function at compile time"));
        // The depth counter below is the limit a program is judged against,
        // but it is only safe while limits_.max_recursion_depth levels
        // actually fit on the host stack. Measure the bytes as well, so the
        // engine reports this diagnostic rather than dying when they do not
        // -- if the per-level frame cost grows, or the host stack is smaller
        // than the 8 MiB max_stack_bytes was derived against. `stack_probe`
        // is an ordinary local: its address lies inside this frame, so the
        // distance from the outermost call's frame is the stack this walk
        // has consumed. The stack grows downward on every target scpp
        // supports; the ordering test below simply declines to measure if it
        // ever does not.
        const char stack_probe = 0;
        if (call_depth_ == 0) {
            stack_base_ = &stack_probe;
        } else if (stack_base_ != nullptr && stack_base_ > &stack_probe &&
                   static_cast<std::size_t>(stack_base_ - &stack_probe) > limits_.max_stack_bytes) {
            return std::unexpected(ConstexprError(loc, "constexpr evaluation exceeded recursion budget"));
        }
        ++call_depth_;
        if (call_depth_ > limits_.max_recursion_depth)
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                --call_depth_;
                return std::unexpected(ConstexprError(loc, "constexpr evaluation exceeded recursion budget"));
            }();
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (fn.params[i].type.kind == TypeKind::Reference) continue;
            if (auto result = reject_user_defined_destructor_execution(fn.params[i].type, loc); !result.has_value()) {
                --call_depth_;
                return std::unexpected(std::move(result).error());
            }
        }
        frames_.emplace_back();
        // ch11 §11.5: while the callee's body runs, unqualified names in
        // it resolve from the callee's own namespace, not the caller's.
        std::vector<std::string> saved_namespace_path = enter_namespace(fn.namespace_path);
        std::unordered_map<std::string, Binding>& frame = frames_.back();
        for (std::size_t i = 0; i < fn.params.size(); ++i) frame.emplace(fn.params[i].name, std::move(bindings[i]));
        auto init_result = execute_constructor_member_initializers(fn);
        std::expected<ExecOutcome, ConstexprError> body_result{};
        if (init_result.has_value()) {
            body_result = execute_stmt(*fn.body, fn.return_type);
        }
        leave_namespace(saved_namespace_path);
        frames_.pop_back();
        --call_depth_;
        if (!init_result.has_value()) return std::unexpected(std::move(init_result).error());
        if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
        if (body_result.value().flow == ExecFlow::Return)
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                if (body_result.value().return_value) return clone_cell(body_result.value().return_value);
                return make_default_cell(fn.return_type, loc);
            }();
        if (is_named_type(fn.return_type, "void")) {
            auto result = std::make_shared<Cell>();
            result->type = named_type("void");
            return result;
        }
        return make_default_cell(fn.return_type, loc);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> call_with_expr_arg_views(const Function& fn, const std::vector<const Expr*>& args,
                                                                 const SourceLocation& loc) {
        std::vector<Binding> bindings{};
        bindings.reserve(fn.params.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            auto bind_result = [&, this]() -> std::expected<void, ConstexprError> {
                const Param& param = fn.params[i];
                const Expr& arg_expr = *args[i];
                if (param.type.kind == TypeKind::Reference) {
                    if (param.type.is_rvalue_ref) {
                        auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        std::shared_ptr<Cell> value = std::move(value_result).value();
                        if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, value->type) &&
                            !types_equal(*param.type.pointee, value->type)) {
                            auto cloned = clone_cell_as_type(value, *param.type.pointee, loc);
                            if (!cloned.has_value()) return std::unexpected(std::move(cloned).error());
                            value = std::move(cloned).value();
                        }
                        bindings.push_back(Binding{value, false});
                        return {};
                    }
                    if (param.type.is_mutable_ref) {
                        auto arg_result = resolve_lvalue(arg_expr);
                        if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                        LValue arg = std::move(arg_result).value();
                        if (arg.read_only) {
                            std::string message{};
                            message += "cannot bind a const/constexpr value to mutable reference parameter '";
                            message += param.name;
                            message += "'";
                            return std::unexpected(ConstexprError(loc, message));
                        }
                        if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, arg.cell->type) &&
                            !types_equal(*param.type.pointee, arg.cell->type)) {
                            auto aliased = alias_cell_as_type(arg.cell, *param.type.pointee, loc);
                            if (!aliased.has_value()) return std::unexpected(std::move(aliased).error());
                            bindings.push_back(Binding{std::move(aliased).value(), false});
                        } else {
                            bindings.push_back(Binding{arg.cell, false});
                        }
                    } else {
                        // Speculative: prefer binding through the argument's
                        // real lvalue storage (possibly aliased to a base-class
                        // reference type); if either resolving the lvalue or
                        // that aliasing fails, fall back to plain evaluation,
                        // exactly like the original try/catch(const
                        // ConstexprError&) fallback (which covered both steps).
                        std::shared_ptr<Cell> bound_value{};
                        bool can_bind_lvalue = false;
                        auto arg_lvalue_result = resolve_lvalue(arg_expr);
                        if (arg_lvalue_result.has_value()) {
                            LValue arg = std::move(arg_lvalue_result).value();
                            if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, arg.cell->type) &&
                                !types_equal(*param.type.pointee, arg.cell->type)) {
                                auto aliased = alias_cell_as_type(arg.cell, *param.type.pointee, loc);
                                if (aliased.has_value()) {
                                    bound_value = std::move(aliased).value();
                                    can_bind_lvalue = true;
                                }
                            } else {
                                bound_value = arg.cell;
                                can_bind_lvalue = true;
                            }
                        }
                        if (can_bind_lvalue) {
                            bindings.push_back(Binding{std::move(bound_value), true});
                        } else {
                            auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            std::shared_ptr<Cell> value = std::move(value_result).value();
                            if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, value->type) &&
                                !types_equal(*param.type.pointee, value->type)) {
                                auto cloned = clone_cell_as_type(value, *param.type.pointee, loc);
                                if (!cloned.has_value()) return std::unexpected(std::move(cloned).error());
                                value = std::move(cloned).value();
                            }
                            bindings.push_back(Binding{value, true});
                        }
                    }
                } else {
                    auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    std::shared_ptr<Cell> value = std::move(value_result).value();
                    if (is_same_or_base_class_type(param.type, value->type) && !types_equal(param.type, value->type)) {
                        auto cloned = clone_cell_as_type(value, param.type, loc);
                        if (!cloned.has_value()) return std::unexpected(std::move(cloned).error());
                        bindings.push_back(Binding{std::move(cloned).value(), false});
                    } else if (!types_equal(param.type, value->type) &&
                               param.type.kind == TypeKind::Named && is_record_name(param.type.name)) {
                        OptionalFunctionRef ctor_ref =
                            find_single_argument_converting_constructor(param.type.name, value);
                        if (!ctor_ref.has_value()) {
                            std::string message{};
                            message += "constexpr call has no viable converting constructor for parameter '";
                            message += param.name;
                            message += "'";
                            return std::unexpected(ConstexprError(loc, message));
                        }
                        auto object_result = make_default_cell(param.type, loc);
                        if (!object_result.has_value()) return std::unexpected(std::move(object_result).error());
                        auto object = std::move(object_result).value();
                        std::vector<Binding> ctor_bindings{};
                        ctor_bindings.push_back(Binding{object, false});
                        ctor_bindings.push_back(Binding{value, false});
                        auto ctor_call_result = call_function(ctor_ref->get(), std::move(ctor_bindings), loc);
                        if (!ctor_call_result.has_value()) return std::unexpected(std::move(ctor_call_result).error());
                        bindings.push_back(Binding{object, false});
                    } else {
                        bindings.push_back(Binding{value, false});
                    }
                }
                    return {};
            }();
            if (!bind_result.has_value()) return std::unexpected(std::move(bind_result).error());
        }
        return call_function(fn, std::move(bindings), loc);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> call_with_expr_args(const Function& fn, const std::vector<ExprPtr>& args,
                                                            const SourceLocation& loc) {
        std::vector<const Expr*> arg_views{};
        arg_views.reserve(args.size());
        for (const ExprPtr& arg : args) arg_views.push_back(arg.get());
        return call_with_expr_arg_views(fn, arg_views, loc);
    }

    [[nodiscard]] std::expected<OptionalFunctionRef, ConstexprError> find_method_callable(const Expr& receiver_expr, const std::string& method_name,
                                                       const std::vector<std::shared_ptr<Cell>>& arg_values,
                                                       const std::vector<ExprPtr>* arg_exprs = nullptr,
                                                       std::vector<const Function*>* out_ambiguous = nullptr) {
        if (out_ambiguous != nullptr) out_ambiguous->clear();
        std::shared_ptr<Cell> receiver_value{};
        bool receiver_is_lvalue = false;
        bool receiver_read_only = false;
        // Speculative: prefer resolving the receiver as a real lvalue (so
        // a mutable-this method can be found); a receiver that isn't
        // itself an lvalue (e.g. a temporary) falls back to plain
        // evaluation, exactly like the original try/catch(const
        // ConstexprError&) fallback. A genuine failure from that fallback
        // evaluation is a real error and must now propagate explicitly.
        auto receiver_lvalue_result = resolve_lvalue(receiver_expr);
        if (receiver_lvalue_result.has_value()) {
            LValue receiver = std::move(receiver_lvalue_result).value();
            receiver_value = receiver.cell;
            receiver_is_lvalue = true;
            receiver_read_only = receiver.read_only;
        } else {
            auto receiver_value_result = evaluate_expr(receiver_expr);
            if (!receiver_value_result.has_value()) return std::unexpected(std::move(receiver_value_result).error());
            receiver_value = std::move(receiver_value_result).value();
        }
        // A `struct` receiver used to lose its entire candidate set here:
        // is_class_name answers the class-vs-struct *access* question, and
        // it is not the question a method call asks. codegen's
        // collect_call_candidates has always matched on the record, so
        // `constexpr S s{0}; s.m();` reported "no constexpr/consteval
        // overload of method 'm' matches" for a struct while the
        // identical class compiled.
        if (receiver_value->type.kind != TypeKind::Named || !is_record_name(receiver_value->type.name)) return {};

        std::string full_name{};
        full_name += receiver_value->type.name;
        full_name += "_";
        full_name += method_name;
        if (!functions_by_name_.contains(full_name)) return {};
        std::vector<const Function*> matches{};
        for (std::size_t fn_index : functions_by_name_.at(full_name)) {
            const Function& fn = program_.functions[fn_index];
            if (fn.params.size() != arg_values.size() + 1 || fn.params.empty()) continue;

            const Type& this_type = fn.params[0].type;
            if (this_type.kind == TypeKind::Reference) {
                if (!this_type.pointee) continue;
                Type receiver_expected = *this_type.pointee;
                receiver_expected.is_const_qualified = false;
                if (!is_same_or_base_class_type(receiver_expected, receiver_value->type)) continue;
                if (this_type.is_mutable_ref && (!receiver_is_lvalue || receiver_read_only)) continue;
            } else if (!is_same_or_base_class_type(this_type, receiver_value->type)) {
                continue;
            }

            bool params_match = true;
            for (std::size_t i = 0; i < arg_values.size(); ++i) {
                const Expr* arg_expr = arg_exprs != nullptr && i < arg_exprs->size() ? (*arg_exprs)[i].get() : nullptr;
                if (arg_expr != nullptr && arg_expr->kind == ExprKind::BracedInitList) {
                    if (!braced_init_list_can_initialize(fn.params[i + 1].type, arg_expr->args, arg_expr->loc)) {
                        params_match = false;
                        break;
                    }
                    continue;
                }
                if (!constexpr_argument_matches_parameter(fn.params[i + 1].type, arg_values[i])) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) matches.push_back(&fn);
        }
        return best_candidate(matches, arg_values, arg_exprs, /*param_offset=*/1, out_ambiguous);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_constructor_expr(const Expr& expr) {
        Type object_type = named_type(expr.name);
        auto object_result = make_default_cell(object_type, expr.loc);
        if (!object_result.has_value()) return std::unexpected(std::move(object_result).error());
        auto object = std::move(object_result).value();
        // See the VarDecl path's note: a braced-list element has no type
        // to resolve a constructor with, so the list is aggregate
        // initialization and is decided before the arguments are
        // evaluated.
        if (args_contain_braced_init_list(expr.args) && record_is_aggregate(expr.name)) {
            if (auto result = initialize_cell_from_brace_args(object, object_type, expr.args, expr.loc);
                !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
            return object;
        }
        std::vector<std::shared_ptr<Cell>> arg_values{};
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) {
            auto arg_result = evaluate_expr(*arg);
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            arg_values.push_back(std::move(arg_result).value());
        }
        std::vector<const Function*> tied_constructors{};
        OptionalFunctionRef ctor_ref = find_constructor(expr.name, arg_values, &tied_constructors);
        if (!ctor_ref.has_value()) {
            if (tied_constructors.size() > 1) {
                return std::unexpected(ConstexprError(expr.loc, ambiguous_candidates_message(
                    "constructor for type '" + expr.name + "'", tied_constructors, expr.name)));
            }
            if (expr.args.empty() &&
                (classes_by_name_.contains(expr.name) || structs_by_name_.contains(expr.name))) {
                if (auto result = apply_default_initializers_to_named_object(object, object_type, expr.loc); !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                return object;
            }
            // [dcl.init.aggr]: a struct with no matching constructor
            // takes its members from the list directly. Reached only
            // after constructor selection has already declined, which is
            // the same order codegen uses.
            if (!expr.args.empty() && record_is_aggregate(expr.name)) {
                const StructDef& struct_def = program_.structs[structs_by_name_.at(expr.name)];
                if (auto result = apply_default_initializers_to_named_object(object, object_type, expr.loc); !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                if (auto result = initialize_record_cell_from_brace_args(object, struct_def, expr.args, expr.loc);
                    !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                return object;
            }
            std::string message{};
            message += "no constexpr/consteval constructor matches for type '";
            message += expr.name;
            message += "'";
            return std::unexpected(ConstexprError(expr.loc, message));
        }
        const Function& ctor = ctor_ref->get();
        std::vector<Binding> bindings{};
        bindings.reserve(ctor.params.size());
        bindings.push_back(Binding{object, false});
        for (std::size_t i = 1; i < ctor.params.size(); ++i) {
            const Param& param = ctor.params[i];
            const Expr& arg_expr = *expr.args[i - 1];
            if (param.type.kind == TypeKind::Reference) {
                if (param.type.is_rvalue_ref) {
                    auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    bindings.push_back(Binding{std::move(value_result).value(), false});
                    continue;
                }
                if (param.type.is_mutable_ref) {
                    auto arg_result = resolve_lvalue(arg_expr);
                    if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                    bindings.push_back(Binding{arg_result.value().cell, false});
                } else {
                    // Speculative: prefer binding through the argument's
                    // real lvalue storage; a non-lvalue expression falls
                    // back to plain evaluation, exactly like the original
                    // try/catch(const ConstexprError&) fallback.
                    auto arg_lvalue_result = resolve_lvalue(arg_expr);
                    if (arg_lvalue_result.has_value()) {
                        bindings.push_back(Binding{arg_lvalue_result.value().cell, true});
                    } else {
                        auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        bindings.push_back(Binding{std::move(value_result).value(), true});
                    }
                }
            } else {
                auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                bindings.push_back(Binding{std::move(value_result).value(), false});
            }
        }
        auto call_result = call_function(ctor, std::move(bindings), expr.loc);
        if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
        return object;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_call_expr(const Expr& expr) {
        if (expr.lhs)
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                std::vector<std::shared_ptr<Cell>> arg_values{};
                arg_values.reserve(expr.args.size());
                for (const ExprPtr& arg : expr.args) {
                    // See the free-function path below: a braced-list
                    // argument is left unevaluated and answered from its
                    // expression against each candidate's parameter type.
                    if (arg != nullptr && arg->kind == ExprKind::BracedInitList) {
                        arg_values.push_back(nullptr);
                        continue;
                    }
                    auto arg_result = evaluate_expr(*arg);
                    if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                    arg_values.push_back(std::move(arg_result).value());
                }
                std::vector<const Function*> tied_methods{};
                auto fn_result =
                    find_method_callable(*expr.lhs, expr.name, arg_values, &expr.args, &tied_methods);
                if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
                OptionalFunctionRef method_ref = fn_result.value();
                if (!method_ref.has_value()) {
                    if (tied_methods.size() > 1) {
                        return std::unexpected(ConstexprError(expr.loc, ambiguous_candidates_message(
                            "method '" + expr.name + "'", tied_methods, expr.name)));
                    }
                    std::string message{};
                    message += "no constexpr/consteval overload of method '";
                    message += expr.name;
                    message += "' matches this immediate call";
                    return std::unexpected(ConstexprError(expr.loc, message));
                }
                std::vector<const Expr*> all_args{};
                all_args.reserve(expr.args.size() + 1);
                all_args.push_back(expr.lhs.get());
                for (const ExprPtr& arg : expr.args) all_args.push_back(arg.get());
                return call_with_expr_arg_views(method_ref->get(), all_args, expr.loc);
            }();
        if (is_record_name(expr.name)) return evaluate_constructor_expr(expr);
        std::vector<std::shared_ptr<Cell>> arg_values{};
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) {
            // A braced-list argument is deliberately left unevaluated
            // (a null placeholder, keeping the vector index-aligned):
            // it has no type of its own to pre-evaluate into, and the
            // overload-selection loops it feeds consult the argument
            // *expressions* for exactly these positions instead.
            if (arg != nullptr && arg->kind == ExprKind::BracedInitList) {
                arg_values.push_back(nullptr);
                continue;
            }
            auto arg_result = evaluate_expr(*arg);
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            arg_values.push_back(std::move(arg_result).value());
        }
        std::vector<const Function*> tied_callees{};
        OptionalFunctionRef callee_ref =
            find_callable(expr.name, arg_values, expr.explicit_global_qualification, &expr.args, &tied_callees);
        if (!callee_ref.has_value())
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                if (tied_callees.size() > 1) {
                    return std::unexpected(ConstexprError(expr.loc, ambiguous_candidates_message(
                        "call to '" + expr.name + "'", tied_callees, expr.name)));
                }
                std::string message{};
                message += "no constexpr/consteval overload of '";
                message += expr.name;
                message += "' matches this immediate call";
                return std::unexpected(ConstexprError(expr.loc, message));
            }();
        return call_with_expr_args(callee_ref->get(), expr.args, expr.loc);
    }

    // Forwards to scpp::find_enum_variant_index (scpp.ast), the one
    // enumerator-by-name lookup, which movecheck and codegen also ask.
    [[nodiscard]] std::optional<std::reference_wrapper<const EnumDef>> find_enum_for_variant(const std::string& variant_name) const {
        std::optional<EnumVariantIndex> found = find_enum_variant_index(program_, variant_name);
        if (!found.has_value()) return {};
        const EnumDef& def = program_.enums[found->enum_index];
        return std::optional<std::reference_wrapper<const EnumDef>>{std::reference_wrapper<const EnumDef>{def}};
    }

    // The enumerator's *value*, which the constant evaluator previously
    // had no way to reach: find_enum_for_variant above was consulted only
    // from type inference, so `static_cast<std::int64_t>(E::B)` in a
    // constant expression was given the type `E` and then reported as
    // "identifier 'E::B' is not available". Codegen has always folded the
    // same expression -- two layers, one question, two answers.
    [[nodiscard]] std::shared_ptr<Cell> enumerator_cell(const std::string& variant_name) const {
        std::optional<EnumVariantIndex> found = find_enum_variant_index(program_, variant_name);
        if (!found.has_value()) return nullptr;
        const EnumDef& def = program_.enums[found->enum_index];
        auto cell = std::make_shared<Cell>();
        cell->type = named_type(def.name);
        cell->data.set_integer(def.variants[found->variant_index].value);
        return cell;
    }

    [[nodiscard]] std::optional<Type> infer_unevaluated_expr_type(const Expr& expr) {
        // A braced-init-list has no type of its own; only the
        // initialization boundary that consumes it knows what it means.
        if (expr.kind == ExprKind::BracedInitList) return std::nullopt;
        switch (expr.kind) {
            case ExprKind::BracedInitList: return std::nullopt;
            case ExprKind::IntegerLiteral: return named_type("int");
            case ExprKind::FloatLiteral: return named_type("double");
            case ExprKind::BoolLiteral: return named_type("bool");
            case ExprKind::NullptrLiteral: return nullptr_named_type();
            case ExprKind::CharLiteral: return named_type("char");
            case ExprKind::TypeTrait: return named_type("bool");
            case ExprKind::Alignof:
            case ExprKind::Sizeof:
            return [&]() -> std::optional<Type> {
                    return named_type("size_t");
            }();
            case ExprKind::ValueInit:
            return [&]() -> std::optional<Type> {
                    return expr.type;
            }();
            case ExprKind::Destroy: return named_type("void");
            case ExprKind::StringLiteral: return string_literal_type(expr.name.size());
            case ExprKind::Identifier:
                return [&, this]() -> std::optional<Type> {
                    // Speculative: prefer a real binding; an identifier that
                    // isn't currently bound (e.g. an enum variant name used
                    // as a value) falls back to enum-variant lookup, else
                    // nullopt -- exactly like the original try/catch(const
                    // ConstexprError&) fallback.
                    auto binding_result = lookup_binding(expr.name, expr.loc, expr.explicit_global_qualification);
                    if (binding_result.has_value()) return binding_result.value().cell->type;
                    if (std::optional<std::reference_wrapper<const EnumDef>> enum_def = find_enum_for_variant(expr.name);
                        enum_def.has_value()) {
                        return named_type(enum_def->get().name);
                    }
                    return std::nullopt;
                }();
            case ExprKind::Move:
            return [&, this]() -> std::optional<Type> {
                    return expr.lhs ? infer_unevaluated_expr_type(*expr.lhs) : std::nullopt;
            }();
            case ExprKind::New:
                return [&]() -> std::optional<Type> {
                    Type result{};
                    result.kind = TypeKind::Pointer;
                    result.pointee = std::make_shared<Type>(expr.type);
                    result.is_mutable_pointee = true;
                    return result;
                }();
            case ExprKind::Delete:
            return [&]() -> std::optional<Type> {
                    return named_type("void");
            }();
            case ExprKind::Cast:
            return [&]() -> std::optional<Type> {
                    return expr.type;
            }();
            case ExprKind::Lambda:
            return [&]() -> std::optional<Type> {
                    return expr.name.empty() ? std::nullopt : std::optional<Type>(named_type(expr.name));
            }();
            case ExprKind::Conditional:
                return [&, this]() -> std::optional<Type> {
                    if (!expr.rhs || !expr.third) return std::nullopt;
                    std::optional<Type> lhs_type = infer_unevaluated_expr_type(*expr.rhs);
                    std::optional<Type> rhs_type = infer_unevaluated_expr_type(*expr.third);
                    if (!lhs_type.has_value() || !rhs_type.has_value()) return std::nullopt;
                    // [expr.cond]/4 applies the array-to-pointer conversion to
                    // both operands before the composite type is determined.
                    lhs_type = decay_array_to_pointer(*lhs_type);
                    rhs_type = decay_array_to_pointer(*rhs_type);
                    if (!types_equal(*lhs_type, *rhs_type)) return std::nullopt;
                    return lhs_type;
                }();
            case ExprKind::Member:
                return [&, this]() -> std::optional<Type> {
                    std::optional<Type> base = infer_unevaluated_expr_type(*expr.lhs);
                    if (!base.has_value()) return std::nullopt;
                    bool base_is_sized_span = base->kind == TypeKind::Span && base->pointee != nullptr;
                    if (base_is_sized_span && expr.name == "size") {
                        return named_type("size_t");
                    }
                    const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
                    if (base_named.kind != TypeKind::Named) return std::nullopt;
                    if (structs_by_name_.contains(base_named.name)) {
                        const StructDef& struct_def = program_.structs[structs_by_name_.at(base_named.name)];
                        for (const StructField& field : struct_def.fields) {
                            if (field.name == expr.name) return field.type;
                        }
                    }
                    if (classes_by_name_.contains(base_named.name)) {
                        // Best-effort: a missing base-class definition is a
                        // real diagnostic elsewhere (e.g. make_default_cell);
                        // here it just means this type can't be inferred.
                        const ClassDef& class_def = program_.classes[classes_by_name_.at(base_named.name)];
                        auto fields_result = collect_class_fields(class_def);
                        if (fields_result.has_value()) {
                            for (const ClassField& field : fields_result.value()) {
                                if (field.name == expr.name) return field.type.kind == TypeKind::Reference ? *field.type.pointee : field.type;
                            }
                        }
                    }
                    return std::nullopt;
                }();
            case ExprKind::Subscript:
                return [&, this]() -> std::optional<Type> {
                    std::optional<Type> base = infer_unevaluated_expr_type(*expr.lhs);
                    if (!base.has_value()) return std::nullopt;
                    const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
                    if (effective.kind == TypeKind::Array && effective.element) return *effective.element;
                    if ((effective.kind == TypeKind::Pointer || effective.kind == TypeKind::Span) && effective.pointee) {
                        return *effective.pointee;
                    }
                    return std::nullopt;
                }();
            case ExprKind::Unary:
            return [&, this]() -> std::optional<Type> {
                    if (!expr.lhs) return std::nullopt;
                    switch (expr.unary_op) {
                        case UnaryOp::Neg: return infer_unevaluated_expr_type(*expr.lhs);
                        case UnaryOp::Not: return named_type("bool");
                        case UnaryOp::PreInc:
                        case UnaryOp::PreDec:
                        case UnaryOp::PostInc:
                        case UnaryOp::PostDec:
                            return infer_unevaluated_expr_type(*expr.lhs);
                        case UnaryOp::Deref: {
                            std::optional<Type> operand = infer_unevaluated_expr_type(*expr.lhs);
                            if (!operand.has_value()) return std::nullopt;
                            // [expr.unary.op]/1 requires a pointer operand,
                            // which an array reaches through [conv.array]'s
                            // array-to-pointer conversion.
                            if (operand->kind == TypeKind::Array) operand = decay_array_to_pointer(*operand);
                            if ((operand->kind == TypeKind::Pointer || operand->kind == TypeKind::Reference) && operand->pointee) {
                                return *operand->pointee;
                            }
                            return std::nullopt;
                        }
                        case UnaryOp::AddressOf: {
                            std::optional<Type> operand = infer_unevaluated_expr_type(*expr.lhs);
                            if (!operand.has_value()) return std::nullopt;
                            return make_pointer_type_to(*operand, true);
                        }
                    }
                    return std::nullopt;
            }();
            case ExprKind::Binary:
            return [&, this]() -> std::optional<Type> {
                    if (!expr.lhs || !expr.rhs) return std::nullopt;
                    if (expr.binary_op == BinaryOp::Assign || expr.binary_op == BinaryOp::AddAssign ||
                        expr.binary_op == BinaryOp::SubAssign || expr.binary_op == BinaryOp::MulAssign ||
                        expr.binary_op == BinaryOp::DivAssign) {
                        return infer_unevaluated_expr_type(*expr.lhs);
                    }
                    if (expr.binary_op == BinaryOp::Eq || expr.binary_op == BinaryOp::Ne || expr.binary_op == BinaryOp::Lt ||
                        expr.binary_op == BinaryOp::Gt || expr.binary_op == BinaryOp::Le || expr.binary_op == BinaryOp::Ge ||
                        expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) {
                        return named_type("bool");
                    }
                    return infer_unevaluated_expr_type(*expr.lhs);
            }();
            case ExprKind::Call:
            return [&, this]() -> std::optional<Type> {
                    if (expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1) {
                        std::optional<Type> range_type = infer_unevaluated_expr_type(*expr.args[0]);
                        if (!range_type.has_value()) return std::nullopt;
                        const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                                    ? *range_type->pointee
                                                    : *range_type;
                        if (unwrapped.kind == TypeKind::Array || unwrapped.kind == TypeKind::Span) return named_type("int");
                        return std::nullopt;
                    }
                    if (expr.lhs) {
                        std::optional<Type> receiver = infer_unevaluated_expr_type(*expr.lhs);
                        const Type& receiver_named = receiver.has_value() && receiver->kind == TypeKind::Reference ? *receiver->pointee
                                                                                                                     : *receiver;
                        if (!receiver.has_value() || receiver_named.kind != TypeKind::Named) return std::nullopt;
                        std::string full_name{};
                        full_name += receiver_named.name;
                        full_name += "_";
                        full_name += expr.name;
                        if (!functions_by_name_.contains(full_name)) return std::nullopt;
                        for (std::size_t fn_index : functions_by_name_.at(full_name)) {
                            const Function& fn = program_.functions[fn_index];
                            if (fn.params.size() == expr.args.size() + 1) return fn.return_type;
                        }
                        return std::nullopt;
                    }
                    if (is_record_name(expr.name)) return named_type(expr.name);
                    if (functions_by_name_.contains(expr.name)) {
                        for (std::size_t fn_index : functions_by_name_.at(expr.name)) {
                            const Function& fn = program_.functions[fn_index];
                            if (fn.params.size() == expr.args.size()) return fn.return_type;
                        }
                    }
                    return std::nullopt;
            }();
            case ExprKind::PackExpansion:
            case ExprKind::Fold:
            return [&]() -> std::optional<Type> {
                    return std::nullopt;
            }();
        }
        return std::nullopt;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_expr(const Expr& expr) {
        return evaluate_expr_in_context(expr, nullptr);
    }

    // ch06 §6: a literal has no type of its own, it adopts the type of
    // the place that consumes it. Move checking has always implemented
    // that rule; constant evaluation did not -- every integer literal
    // became an `int` and every floating literal a `double` here, so
    // `constexpr int8_t v = 5;` passed move checking and was then
    // rejected by this evaluator as an int-to-int8_t assignment. The two
    // layers disagreed about which programs are valid.
    //
    // `context_type` is the type of the place the expression is being
    // evaluated for, or null where there is no such place (a discarded
    // expression statement, a condition). Both layers now decide
    // adoption with `scpp.ast`'s `literal_adopts_type`, so they cannot
    // drift apart again.
    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_expr_in_context(const Expr& expr, const Type* context_type) {
        if (auto result = tick(expr.loc, "evaluating an expression"); !result.has_value()) return std::unexpected(std::move(result).error());
        if (context_type != nullptr && is_untyped_numeric_literal(expr) &&
            literal_adopts_type(expr, *context_type, scpp::host_pointer_bit_width())) {
            return make_adopted_literal_cell(expr, literal_adoption_target(*context_type));
        }
        if (expr.kind == ExprKind::BracedInitList) {
            // A nested list is bound to whatever the enclosing
            // initialization says it is. Every braced-list element --
            // array element and struct member alike -- is evaluated
            // through this one function against its target's declared
            // type, so handling the nested list here is what makes
            // nesting work at every depth and in every shape.
            if (context_type == nullptr) {
                return std::unexpected(ConstexprError(
                    expr.loc,
                    "a brace-enclosed initializer list has no type of its own and cannot be used here"));
            }
            auto cell_result = make_default_cell(*context_type, expr.loc);
            if (!cell_result.has_value()) return std::unexpected(std::move(cell_result).error());
            auto cell = std::move(cell_result).value();
            if (auto result = initialize_cell_from_brace_args(cell, *context_type, expr.args, expr.loc);
                !result.has_value()) {
                return std::unexpected(std::move(result).error());
            }
            return cell;
        }
        switch (expr.kind) {
            case ExprKind::BracedInitList:
                return std::unexpected(ConstexprError(
                    expr.loc, "a brace-enclosed initializer list has no type of its own and cannot be used here"));
            case ExprKind::IntegerLiteral: return make_scalar_cell(named_type("int"), expr.int_value);
            case ExprKind::FloatLiteral: return make_double_cell(expr.float_value);
            case ExprKind::BoolLiteral: return make_bool_cell(expr.bool_value);
            case ExprKind::NullptrLiteral: return make_nullptr_cell();
            case ExprKind::CharLiteral: return make_scalar_cell(named_type("char"), expr.int_value);
            case ExprKind::StringLiteral: return make_string_literal_pointer(expr);
            case ExprKind::Destroy:
            return [&]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    return std::unexpected(ConstexprError(expr.loc, "explicit destructor calls are not supported during constant evaluation"));
            }();
            case ExprKind::ValueInit:
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    return make_default_cell(expr.type, expr.loc);
            }();
            case ExprKind::Alignof:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    if (auto result = reject_if_incomplete(expr.type, expr.loc, "alignof"); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                    std::optional<TypeLayoutInfo> layout = layout_of_type(program_, expr.type);
                    if (!layout.has_value()) return std::unexpected(ConstexprError(expr.loc, "cannot apply 'alignof' to this type in this version"));
                    return make_scalar_cell(named_type("size_t"), static_cast<std::int64_t>(layout->abi_align_bytes));
                }();
            case ExprKind::Sizeof:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    Type queried_type{};
                    if (expr.sizeof_operand_is_type) {
                        queried_type = expr.type;
                    } else {
                        std::optional<Type> inferred = infer_unevaluated_expr_type(*expr.lhs);
                        if (!inferred.has_value()) {
                            return std::unexpected(ConstexprError(expr.loc, "cannot apply 'sizeof' to this expression: its type could not be inferred"));
                        }
                        queried_type = *inferred;
                    }
                    if (auto result = reject_if_incomplete(queried_type, expr.loc, "sizeof"); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                    std::optional<TypeLayoutInfo> layout = layout_of_type(program_, queried_type);
                    if (!layout.has_value()) {
                        return std::unexpected(ConstexprError(expr.loc, "cannot apply 'sizeof' to this type in this version"));
                    }
                    return make_scalar_cell(named_type("size_t"), static_cast<std::int64_t>(layout->size_bytes));
                }();
            case ExprKind::Identifier:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    auto binding_result = lookup_binding(expr.name, expr.loc, expr.explicit_global_qualification);
                    if (binding_result.has_value()) return clone_cell(binding_result.value().cell);
                    // An enumerator names a value, not a binding
                    // ([dcl.enum]/1), and infer_unevaluated_expr_type
                    // already falls back this way for the same reason.
                    if (std::shared_ptr<Cell> enumerator = enumerator_cell(expr.name); enumerator != nullptr) return enumerator;
                    return std::unexpected(std::move(binding_result).error());
                }();
            case ExprKind::Conditional:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    auto cond_result = evaluate_expr(*expr.lhs);
                    if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                    auto cond_bool = as_bool(cond_result.value(), expr.loc);
                    if (!cond_bool.has_value()) return std::unexpected(std::move(cond_bool).error());
                    return cond_bool.value() ? evaluate_expr_in_context(*expr.rhs, context_type)
                                             : evaluate_expr_in_context(*expr.third, context_type);
                }();
            case ExprKind::Member:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    auto base_result = evaluate_expr(*expr.lhs);
                    if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                    std::shared_ptr<Cell> base = std::move(base_result).value();
                    if (base->data.is_span() && expr.name == "size") {
                        return make_checked_int_cell(base->data.span.size, expr.loc);
                    }
                    auto lvalue_result = resolve_lvalue(expr);
                    if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                    return clone_cell(lvalue_result.value().cell);
                }();
            case ExprKind::Subscript:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    auto lvalue_result = resolve_lvalue(expr);
                    if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                    return clone_cell(lvalue_result.value().cell);
                }();
            case ExprKind::Call:
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    if (expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1) {
                        std::optional<Type> range_type = infer_unevaluated_expr_type(*expr.args[0]);
                        if (!range_type.has_value()) return std::unexpected(ConstexprError(expr.loc, "cannot determine range-for operand type"));
                        const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                                    ? *range_type->pointee
                                                    : *range_type;
                        if (unwrapped.kind == TypeKind::Array) return make_checked_int_cell(unwrapped.array_size, expr.loc);
                        if (unwrapped.kind == TypeKind::Span) {
                            auto value_result = evaluate_expr(*expr.args[0]);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            std::shared_ptr<Cell> value = std::move(value_result).value();
                            if (value->data.is_span()) {
                                return make_checked_int_cell(value->data.span.size, expr.loc);
                            }
                        }
                        return std::unexpected(ConstexprError(expr.loc, "range-for requires a fixed-size array or std::span operand"));
                    }
                    return evaluate_call_expr(expr);
            }();
            case ExprKind::Cast:
                return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    // The operand is evaluated with no type context. ch06
                    // §16.2(1) lists exactly the contexts that give a
                    // literal its type -- the entity it initializes, the
                    // parameter it is an argument for, the return type it
                    // is returned from, the other operand of a binary
                    // operator or arm of a conditional -- and a cast
                    // operand is in none of them; §16.2(3) therefore makes
                    // it `int`. Handing it `expr.type` also gave literals
                    // types §16.2(2) forbids outright (`bool` and `char`),
                    // so `static_cast<char>(200)` was read as the `char`
                    // literal 200 and rejected as an overflow rather than
                    // converted to -56.
                    auto operand_result = evaluate_expr(*expr.lhs);
                    if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                    return cast_value(expr.type, operand_result.value(), expr.loc);
                }();
            case ExprKind::Binary:
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    if (expr.binary_op == BinaryOp::Assign || expr.binary_op == BinaryOp::AddAssign ||
                        expr.binary_op == BinaryOp::SubAssign || expr.binary_op == BinaryOp::MulAssign ||
                        expr.binary_op == BinaryOp::DivAssign) {
                        auto target_result = resolve_lvalue(*expr.lhs);
                        if (!target_result.has_value()) return std::unexpected(std::move(target_result).error());
                        LValue target = std::move(target_result).value();
                        if (target.read_only) return std::unexpected(ConstexprError(expr.loc, "cannot assign through a const/constexpr binding"));
                        auto value_result = evaluate_expr_in_context(*expr.rhs, &target.cell->type);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        std::shared_ptr<Cell> value = std::move(value_result).value();
                        if (expr.binary_op != BinaryOp::Assign) {
                            std::shared_ptr<Cell> lhs_value = clone_cell(target.cell);
                            BinaryOp arithmetic_op = expr.binary_op == BinaryOp::AddAssign   ? BinaryOp::Add
                                                     : expr.binary_op == BinaryOp::SubAssign ? BinaryOp::Sub
                                                     : expr.binary_op == BinaryOp::MulAssign ? BinaryOp::Mul
                                                                                             : BinaryOp::Div;
                            Expr arithmetic_expr{};
                            arithmetic_expr.binary_op = arithmetic_op;
                            arithmetic_expr.loc = expr.loc;
                            auto arithmetic_result = evaluate_binary_numeric(arithmetic_expr, lhs_value, value);
                            if (!arithmetic_result.has_value()) return std::unexpected(std::move(arithmetic_result).error());
                            value = std::move(arithmetic_result).value();
                        }
                        if (auto result = copy_into(target.cell, value, expr.loc); !result.has_value()) {
                            return std::unexpected(std::move(result).error());
                        }
                        return clone_cell(target.cell);
                    }
                    if (expr.binary_op == BinaryOp::And)
                        return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                        auto lhs_result = evaluate_expr(*expr.lhs);
                        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
                        auto lhs_bool = as_bool(lhs_result.value(), expr.loc);
                        if (!lhs_bool.has_value()) return std::unexpected(std::move(lhs_bool).error());
                        if (!lhs_bool.value()) return make_bool_cell(false);
                        auto rhs_result = evaluate_expr(*expr.rhs);
                        if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                        auto rhs_bool = as_bool(rhs_result.value(), expr.loc);
                        if (!rhs_bool.has_value()) return std::unexpected(std::move(rhs_bool).error());
                        return make_bool_cell(rhs_bool.value());
                    }();
                    if (expr.binary_op == BinaryOp::Or)
                        return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                        auto lhs_result = evaluate_expr(*expr.lhs);
                        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
                        auto lhs_bool = as_bool(lhs_result.value(), expr.loc);
                        if (!lhs_bool.has_value()) return std::unexpected(std::move(lhs_bool).error());
                        if (lhs_bool.value()) return make_bool_cell(true);
                        auto rhs_result = evaluate_expr(*expr.rhs);
                        if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                        auto rhs_bool = as_bool(rhs_result.value(), expr.loc);
                        if (!rhs_bool.has_value()) return std::unexpected(std::move(rhs_bool).error());
                        return make_bool_cell(rhs_bool.value());
                    }();
                    {
                        // ch06 §6, as move checking already reads it: when
                        // one operand is a literal and the other is not, the
                        // literal adopts the typed operand's type -- `a + 1`
                        // for an int8_t `a` is int8_t arithmetic, not a
                        // conversion. That means evaluating the typed side
                        // first so its type is available as the literal's
                        // context; a literal has no side effects, so the
                        // reordering is unobservable.
                        bool lhs_is_literal = is_untyped_numeric_literal(*expr.lhs);
                        bool rhs_is_literal = is_untyped_numeric_literal(*expr.rhs);
                        if (lhs_is_literal && !rhs_is_literal) {
                            auto rhs_result = evaluate_expr_in_context(*expr.rhs, context_type);
                            if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                            std::shared_ptr<Cell> rhs_cell = std::move(rhs_result).value();
                            auto lhs_result = evaluate_expr_in_context(*expr.lhs, &rhs_cell->type);
                            if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
                            return evaluate_binary_numeric(expr, lhs_result.value(), rhs_cell);
                        }
                        auto lhs_result = evaluate_expr_in_context(*expr.lhs, context_type);
                        if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
                        std::shared_ptr<Cell> lhs_cell = std::move(lhs_result).value();
                        const Type* rhs_context = rhs_is_literal ? &lhs_cell->type : context_type;
                        auto rhs_result = evaluate_expr_in_context(*expr.rhs, rhs_context);
                        if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                        return evaluate_binary_numeric(expr, lhs_cell, rhs_result.value());
                    }
            }();
            case ExprKind::Unary:
            return [&, this]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    switch (expr.unary_op) {
                        case UnaryOp::Neg: {
                            auto operand_result = evaluate_expr_in_context(*expr.lhs, context_type);
                            if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                            std::shared_ptr<Cell> operand = std::move(operand_result).value();
                            // Negation does not change the operand's type --
                            // `-x` for an int8_t x is an int8_t. Both arms
                            // used to discard it and produce `double`/`int`,
                            // so `int8_t v = -x;` failed the assignment's
                            // type check even though nothing about it is a
                            // conversion.
                            if (is_floating_like(operand->type)) {
                                auto double_result = as_double(operand, expr.loc);
                                if (!double_result.has_value()) return std::unexpected(std::move(double_result).error());
                                return make_float_cell_as(operand->type, -double_result.value());
                            }
                            auto integer_result = as_integer(operand, expr.loc);
                            if (!integer_result.has_value()) return std::unexpected(std::move(integer_result).error());
                            std::int64_t value = integer_result.value();
                            if (value == int64_min_value) {
                                return std::unexpected(ConstexprError(expr.loc, "constexpr integer overflow"));
                            }
                            Type negated_type = is_named_type(operand->type, "bool") ? named_type("int") : operand->type;
                            return make_checked_int_cell_as(negated_type, -value, expr.loc);
                        }
                        case UnaryOp::Not: {
                            auto operand_result = evaluate_expr(*expr.lhs);
                            if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                            auto bool_result = as_bool(operand_result.value(), expr.loc);
                            if (!bool_result.has_value()) return std::unexpected(std::move(bool_result).error());
                            return make_bool_cell(!bool_result.value());
                        }
                        case UnaryOp::PreInc:
                        case UnaryOp::PreDec:
                        case UnaryOp::PostInc:
                        case UnaryOp::PostDec:
                            break;
                        case UnaryOp::Deref: {
                            auto lvalue_result = resolve_lvalue(expr);
                            if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                            return clone_cell(lvalue_result.value().cell);
                        }
                        case UnaryOp::AddressOf: {
                            auto target_result = resolve_lvalue(*expr.lhs);
                            if (!target_result.has_value()) return std::unexpected(std::move(target_result).error());
                            LValue target = std::move(target_result).value();
                            auto result = std::make_shared<Cell>();
                            result->type = make_pointer_type_to(target.cell->type, !target.read_only);
                            PointerValue pointer{};
                            if (expr.lhs->kind == ExprKind::Subscript && expr.lhs->lhs && expr.lhs->rhs) {
                                auto offset_value_result = evaluate_expr(*expr.lhs->rhs);
                                if (!offset_value_result.has_value()) return std::unexpected(std::move(offset_value_result).error());
                                auto offset_result = as_integer(offset_value_result.value(), expr.loc);
                                if (!offset_result.has_value()) return std::unexpected(std::move(offset_result).error());
                                std::int64_t offset = offset_result.value();
                                // Speculative: prefer resolving the subscript's
                                // base as a real lvalue array; a non-lvalue
                                // base (e.g. a function call returning a
                                // pointer/span) falls back to plain evaluation,
                                // exactly like the original try/catch(const
                                // ConstexprError&) fallback.
                                auto base_lvalue_result = resolve_lvalue(*expr.lhs->lhs);
                                if (base_lvalue_result.has_value()) {
                                    LValue base_lvalue = std::move(base_lvalue_result).value();
                                    if (base_lvalue.cell->data.is_array()) {
                                        pointer.storage = base_lvalue.cell;
                                        pointer.index = offset;
                                    } else {
                                        pointer.storage = target.cell;
                                        pointer.index = 0;
                                    }
                                } else {
                                    auto base_value_result = evaluate_expr(*expr.lhs->lhs);
                                    if (!base_value_result.has_value()) return std::unexpected(std::move(base_value_result).error());
                                    std::shared_ptr<Cell> base_value = std::move(base_value_result).value();
                                    if (base_value->data.is_span()) {
                                        pointer = base_value->data.span.pointer;
                                        pointer.index += offset;
                                    } else if (base_value->data.is_pointer()) {
                                        pointer = base_value->data.pointer;
                                        pointer.index += offset;
                                    } else {
                                        pointer.storage = target.cell;
                                        pointer.index = 0;
                                    }
                                }
                            } else {
                                pointer.storage = target.cell;
                                pointer.index = 0;
                            }
                            result->data.set_pointer(std::move(pointer));
                            return result;
                        }
                    }
                    return std::unexpected(ConstexprError(expr.loc, "this expression kind is not supported during constant evaluation"));
            }();
            case ExprKind::TypeTrait:
            return [&]() -> std::expected<std::shared_ptr<Cell>, ConstexprError> {
                    return std::unexpected(ConstexprError(expr.loc, "constexpr type traits are deferred to a later phase"));
            }();
            case ExprKind::New:
            case ExprKind::Delete:
            case ExprKind::Move:
            case ExprKind::PackExpansion:
            case ExprKind::Lambda:
            case ExprKind::Fold:
                break;
        }
        return std::unexpected(ConstexprError(expr.loc, "this expression kind is not supported during constant evaluation"));
    }

    [[nodiscard]] std::expected<ExecOutcome, ConstexprError> execute_stmt(const Stmt& stmt, const Type& return_type) {
        if (auto result = tick(stmt.loc, "executing a statement"); !result.has_value()) return std::unexpected(std::move(result).error());
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                return [&, this]() -> std::expected<ExecOutcome, ConstexprError> {
                    if (stmt.type.kind == TypeKind::Span) {
                        if (!stmt.init) {
                            return std::unexpected(ConstexprError(stmt.loc, "std::span<const T> must be initialized during constant evaluation"));
                        }
                        auto span_result = bind_read_only_span(stmt.type, *stmt.init, stmt.loc);
                        if (!span_result.has_value()) return std::unexpected(std::move(span_result).error());
                        frames_.back()[stmt.var_name] = Binding{std::move(span_result).value(),
                                                                stmt.is_const || stmt.is_constexpr};
                        return ExecOutcome{};
                    }
                    if (auto result = reject_user_defined_destructor_execution(stmt.type, stmt.loc); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                    auto cell_result = make_default_cell(stmt.type, stmt.loc);
                    if (!cell_result.has_value()) return std::unexpected(std::move(cell_result).error());
                    auto cell = std::move(cell_result).value();
                    if (stmt.has_ctor_args && stmt.type.kind == TypeKind::Array) {
                        if (auto result = initialize_array_cell_from_brace_args(cell, stmt.type, stmt.ctor_args, stmt.loc);
                            !result.has_value()) {
                            return std::unexpected(std::move(result).error());
                        }
                        frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                        return ExecOutcome{};
                    }
                    // An element that is itself a braced list has no
                    // type, so it cannot take part in the constructor
                    // overload resolution below -- and the list can only
                    // mean aggregate initialization. Deciding that here,
                    // before the arguments are evaluated, is what keeps
                    // a nested list from being evaluated with no target
                    // type to give it meaning.
                    if (stmt.has_ctor_args && args_contain_braced_init_list(stmt.ctor_args) &&
                        record_is_aggregate(stmt.type.name)) {
                        if (auto result = initialize_cell_from_brace_args(cell, stmt.type, stmt.ctor_args, stmt.loc);
                            !result.has_value()) {
                            return std::unexpected(std::move(result).error());
                        }
                        frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                        return ExecOutcome{};
                    }
                    if (stmt.has_ctor_args) {
                        std::vector<Binding> ctor_bindings{};
                        ctor_bindings.reserve(stmt.ctor_args.size() + 1);
                        ctor_bindings.push_back(Binding{cell, false});
                        std::vector<std::shared_ptr<Cell>> arg_values{};
                        arg_values.reserve(stmt.ctor_args.size() + 1);
                        arg_values.push_back(cell);
                        for (const ExprPtr& arg : stmt.ctor_args) {
                            auto arg_result = evaluate_expr(*arg);
                            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                            arg_values.push_back(std::move(arg_result).value());
                        }
                        std::string constructor_name{};
                        constructor_name += stmt.type.name;
                        constructor_name += "_new";
                        std::vector<const Function*> tied_constructors{};
                        OptionalFunctionRef ctor_ref = find_callable(
                            constructor_name, arg_values, /*explicit_global_qualification=*/false,
                            /*arg_exprs=*/nullptr, &tied_constructors);
                        if (!ctor_ref.has_value()) {
                            // A tie is the answer, not a miss: falling
                            // through to "no constexpr/consteval
                            // constructor matches" reported the absence
                            // of something that was in fact present
                            // twice, and pre-empted codegen's own
                            // ambiguity diagnostic.
                            if (tied_constructors.size() > 1) {
                                return std::unexpected(ConstexprError(stmt.loc, ambiguous_candidates_message(
                                    "constructor for type '" + stmt.type.name + "'", tied_constructors,
                                    stmt.type.name)));
                            }
                            if (stmt.ctor_args.empty() &&
                                (classes_by_name_.contains(stmt.type.name) || structs_by_name_.contains(stmt.type.name))) {
                                if (auto result = apply_default_initializers_to_named_object(cell, stmt.type, stmt.loc);
                                    !result.has_value()) {
                                    return std::unexpected(std::move(result).error());
                                }
                                frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                                return ExecOutcome{};
                            }
                            // [dcl.init.aggr], reached from the second
                            // of the evaluator's two constructor-call
                            // sites -- see evaluate_constructor_expr's
                            // matching branch. Both delegate to the one
                            // record-aggregate routine so the rule is
                            // stated once.
                            if (!stmt.ctor_args.empty() && record_is_aggregate(stmt.type.name)) {
                                const StructDef& struct_def = program_.structs[structs_by_name_.at(stmt.type.name)];
                                if (auto result = apply_default_initializers_to_named_object(cell, stmt.type, stmt.loc);
                                    !result.has_value()) {
                                    return std::unexpected(std::move(result).error());
                                }
                                if (auto result = initialize_record_cell_from_brace_args(cell, struct_def, stmt.ctor_args, stmt.loc);
                                    !result.has_value()) {
                                    return std::unexpected(std::move(result).error());
                                }
                                frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                                return ExecOutcome{};
                            }
                            std::string message{};
                            message += "no constexpr/consteval constructor matches for type '";
                            message += stmt.type.name;
                            message += "'";
                            return std::unexpected(ConstexprError(stmt.loc, message));
                        }
                        const Function& ctor = ctor_ref->get();
                        for (std::size_t i = 1; i < ctor.params.size(); ++i) {
                            const Param& param = ctor.params[i];
                            const Expr& arg_expr = *stmt.ctor_args[i - 1];
                            if (param.type.kind == TypeKind::Reference && !param.type.is_rvalue_ref && param.type.is_mutable_ref) {
                                auto arg_result = resolve_lvalue(arg_expr);
                                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                                ctor_bindings.push_back(Binding{arg_result.value().cell, false});
                            } else {
                                auto value_result = evaluate_expr_in_context(arg_expr, &param.type);
                                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                                ctor_bindings.push_back(
                                    Binding{std::move(value_result).value(),
                                            param.type.kind == TypeKind::Reference && !param.type.is_mutable_ref});
                            }
                        }
                        auto call_result = call_function(ctor, std::move(ctor_bindings), stmt.loc);
                        if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
                    } else if (stmt.init) {
                        if (auto result = initialize_cell_from_expr(cell, stmt.type, *stmt.init, stmt.loc);
                            !result.has_value()) {
                            return std::unexpected(std::move(result).error());
                        }
                    }
                    frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                    return ExecOutcome{};
                }();
            case StmtKind::Return:
                return [&, this]() -> std::expected<ExecOutcome, ConstexprError> {
                    if (stmt.expr) {
                        auto value_result = evaluate_expr_in_context(*stmt.expr, &return_type);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        return ExecOutcome{ExecFlow::Return, std::move(value_result).value()};
                    }
                    if (is_named_type(return_type, "void")) {
                        return ExecOutcome{ExecFlow::Return, nullptr};
                    }
                    auto default_result = make_default_cell(return_type, stmt.loc);
                    if (!default_result.has_value()) return std::unexpected(std::move(default_result).error());
                    return ExecOutcome{ExecFlow::Return, std::move(default_result).value()};
                }();
            case StmtKind::ExprStmt:
                if (stmt.expr) {
                    auto result = evaluate_expr(*stmt.expr);
                    if (!result.has_value()) return std::unexpected(std::move(result).error());
                }
                return ExecOutcome{};
            case StmtKind::If:
                if (stmt.if_mode == IfMode::ConstevalTrue) {
                    return execute_stmt(*stmt.then_branch, return_type);
                }
                if (stmt.if_mode == IfMode::ConstevalFalse) {
                    if (stmt.else_branch) return execute_stmt(*stmt.else_branch, return_type);
                    return ExecOutcome{};
                }
                {
                    auto condition_result = evaluate_expr(*stmt.condition);
                    if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
                    auto condition_bool = as_bool(condition_result.value(), stmt.loc);
                    if (!condition_bool.has_value()) return std::unexpected(std::move(condition_bool).error());
                    if (condition_bool.value()) {
                        return execute_stmt(*stmt.then_branch, return_type);
                    }
                    if (stmt.else_branch) {
                        return execute_stmt(*stmt.else_branch, return_type);
                    }
                }
                return ExecOutcome{};
            case StmtKind::While:
                return [&, this]() -> std::expected<ExecOutcome, ConstexprError> {
                    int iterations = 0;
                    while (true) {
                        auto condition_result = evaluate_expr(*stmt.condition);
                        if (!condition_result.has_value()) return std::unexpected(std::move(condition_result).error());
                        auto condition_bool = as_bool(condition_result.value(), stmt.loc);
                        if (!condition_bool.has_value()) return std::unexpected(std::move(condition_bool).error());
                        if (!condition_bool.value()) break;
                        ++iterations;
                        if (iterations > limits_.max_loop_iterations) {
                            return std::unexpected(ConstexprError(stmt.loc, "constexpr evaluation exceeded loop-iteration budget"));
                        }
                        auto body_result = execute_stmt(*stmt.then_branch, return_type);
                        if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
                        if (body_result.value().flow == ExecFlow::Break) break;
                        if (body_result.value().flow == ExecFlow::Return) return body_result;
                        // ExecFlow::Continue and ExecFlow::Normal both simply
                        // loop again (matching the original try/catch, where
                        // a caught ContinueSignal and normal completion both
                        // fell through to the while condition re-check).
                    }
                    return ExecOutcome{};
                }();
            case StmtKind::Switch:
                return [&, this]() -> std::expected<ExecOutcome, ConstexprError> {
                    auto condition_value_result = evaluate_expr(*stmt.condition);
                    if (!condition_value_result.has_value()) return std::unexpected(std::move(condition_value_result).error());
                    std::shared_ptr<Cell> condition_value = std::move(condition_value_result).value();
                    auto condition_key_result = switch_match_key(condition_value, stmt.loc);
                    if (!condition_key_result.has_value()) return std::unexpected(std::move(condition_key_result).error());
                    std::int64_t condition_key = condition_key_result.value();
                    frames_.emplace_back();
                    std::expected<ExecOutcome, ConstexprError> result = ExecOutcome{};
                    bool matched = false;
                    for (const SwitchCase& switch_case : stmt.switch_cases) {
                        bool case_matches = false;
                        if (matched) {
                            case_matches = true;
                        } else if (!switch_case.value) {
                            case_matches = true;
                        } else {
                            auto case_value_result = evaluate_expr(*switch_case.value);
                            if (!case_value_result.has_value()) {
                                result = std::unexpected(std::move(case_value_result).error());
                                break;
                            }
                            auto case_key_result = switch_match_key(case_value_result.value(), switch_case.loc);
                            if (!case_key_result.has_value()) {
                                result = std::unexpected(std::move(case_key_result).error());
                                break;
                            }
                            case_matches = case_key_result.value() == condition_key;
                        }
                        if (!case_matches) continue;
                        matched = true;
                        ExecOutcome case_outcome{};
                        bool case_broke = false;
                        for (const StmtPtr& nested : switch_case.statements) {
                            auto nested_result = execute_stmt(*nested, return_type);
                            if (!nested_result.has_value()) {
                                result = std::unexpected(std::move(nested_result).error());
                                break;
                            }
                            case_outcome = nested_result.value();
                            if (case_outcome.flow == ExecFlow::Break) {
                                case_broke = true;
                                break;
                            }
                            if (case_outcome.flow != ExecFlow::Normal) break;
                        }
                        if (!result.has_value()) break;
                        if (case_broke) {
                            // A `break;` inside a switch case is consumed here
                            // (matching the original catch(const
                            // BreakSignal&)): it stops case scanning and the
                            // switch itself completes normally.
                            break;
                        }
                        if (case_outcome.flow == ExecFlow::Return || case_outcome.flow == ExecFlow::Continue) {
                            // Unlike Break, Return/Continue were never caught
                            // by the original Switch's exception handling, so
                            // they must keep propagating to this statement's
                            // own caller unchanged.
                            result = case_outcome;
                            break;
                        }
                        bool ends_with_fallthrough =
                            !switch_case.statements.empty() && switch_case.statements.back()->kind == StmtKind::Fallthrough;
                        if (!ends_with_fallthrough) break;
                    }
                    frames_.pop_back();
                    return result;
                }();
            case StmtKind::Break: return ExecOutcome{ExecFlow::Break, nullptr};
            case StmtKind::Continue: return ExecOutcome{ExecFlow::Continue, nullptr};
            case StmtKind::Fallthrough: return ExecOutcome{};
            case StmtKind::Block:
                return [&, this]() -> std::expected<ExecOutcome, ConstexprError> {
                    if (stmt.is_unsafe) return std::unexpected(ConstexprError(stmt.loc, "unsafe blocks are not allowed in constant evaluation"));
                    frames_.emplace_back();
                    std::expected<ExecOutcome, ConstexprError> result = ExecOutcome{};
                    for (const StmtPtr& nested : stmt.statements) {
                        result = execute_stmt(*nested, return_type);
                        if (!result.has_value() || result.value().flow != ExecFlow::Normal) break;
                    }
                    frames_.pop_back();
                    return result;
                }();
        }
        return ExecOutcome{};
    }
};

[[nodiscard]] bool expr_depends_on_runtime_bindings(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            return true;
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
        case ExprKind::NullptrLiteral:
        case ExprKind::CharLiteral:
        case ExprKind::StringLiteral:
        case ExprKind::TypeTrait:
            break;
        default:
            break;
    }
    if (expr.lhs && expr_depends_on_runtime_bindings(*expr.lhs)) return true;
    if (expr.rhs && expr_depends_on_runtime_bindings(*expr.rhs)) return true;
    if (expr.third && expr_depends_on_runtime_bindings(*expr.third)) return true;
    for (const ExprPtr& arg : expr.args) {
        if (expr_depends_on_runtime_bindings(*arg)) return true;
    }
    for (const ExplicitTemplateArg& arg : expr.explicit_template_args) {
        if (arg.value && expr_depends_on_runtime_bindings(*arg.value)) return true;
    }
    return false;
}

// Writes an immediate function's result back into the AST as the literal
// that spells it. This used to enumerate `int`/`char`/`bool`/`double`
// and reject every other scalar, so a `consteval int8_t f()` evaluated
// fine and then failed to lower. The three literal kinds are now chosen
// from `scpp.ast`'s scalar model, which is also what decides what the
// resulting literal may adopt -- so an int8_t result lowers to an
// integer literal that the int8_t place it fills accepts, and a
// `float32_t` result lowers to a floating literal rather than being
// mislabelled a double.
[[nodiscard]] std::expected<void, ConstexprError> rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value) {
    bool is_named = value->type.kind == TypeKind::Named;
    std::string_view value_type_name{};
    if (is_named) value_type_name = std::string_view{value->type.name};
    if (is_named && value->type.name == "bool") {
        expr.kind = ExprKind::BoolLiteral;
        expr.bool_value = value->data.bool_value;
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.name.clear();
    } else if (is_named && value->type.name == "char") {
        expr.kind = ExprKind::CharLiteral;
        expr.int_value = value->data.int_value;
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named && is_integral_scalar_type_name(value_type_name)) {
        expr.kind = ExprKind::IntegerLiteral;
        expr.int_value = value->data.int_value;
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named && is_float_scalar_type_name(value_type_name)) {
        expr.kind = ExprKind::FloatLiteral;
        expr.float_value = value->data.double_value;
        expr.int_value = 0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (types_equal(value->type, make_const_char_pointer_type())) {
        // The storage null-check is new: this site used to read
        // `pointer->storage->data` while only having tested `pointer`
        // itself, unlike the otherwise-identical span-binding check above
        // which does test `storage`. A PointerValue with null storage
        // would have dereferenced null here.
        if (!value->data.is_pointer() || !value->data.pointer.storage ||
            !value->data.pointer.storage->data.is_array() || value->data.pointer.index != 0) {
            return std::unexpected(ConstexprError(expr.loc, "a constant-evaluated pointer result cannot be written back into source form"));
        }
        const ArrayValue& array = value->data.pointer.storage->data.array;
        expr.kind = ExprKind::StringLiteral;
        expr.name.clear();
        for (const std::shared_ptr<Cell>& element : array.elements) {
            std::int64_t ch = element->data.int_value;
            if (ch == 0) break;
            expr.name.push_back(static_cast<char>(ch));
        }
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.bool_value = false;
    } else {
        return std::unexpected(ConstexprError(expr.loc, "only scalar and string-literal immediate results can be written back into source form"));
    }
    expr.lhs.reset();
    expr.rhs.reset();
    expr.third.reset();
    expr.args.clear();
    expr.explicit_template_args.clear();
    expr.type = value->type;
    return {};
}

[[nodiscard]] std::expected<ConstexprValue, ConstexprError> snapshot_constexpr_value(const std::shared_ptr<Cell>& value, const SourceLocation& loc) {
    ConstexprValue snapshot{};
    snapshot.type = value->type;
    if (is_named_type(value->type, "void")) {
        snapshot.kind = ConstexprValueKind::Void;
        return snapshot;
    }
    if (is_named_type(value->type, "int") || is_named_type(value->type, "char")) {
        snapshot.kind = ConstexprValueKind::Integer;
        snapshot.int_value = value->data.int_value;
        return snapshot;
    }
    if (is_named_type(value->type, "bool")) {
        snapshot.kind = ConstexprValueKind::Bool;
        snapshot.bool_value = value->data.bool_value;
        return snapshot;
    }
    if (is_named_type(value->type, "double")) {
        snapshot.kind = ConstexprValueKind::Double;
        snapshot.double_value = value->data.double_value;
        return snapshot;
    }
    if (types_equal(value->type, make_const_char_pointer_type())) {
        // Same added storage null-check as in rewrite_expr_as_constant.
        if (!value->data.is_pointer() || !value->data.pointer.storage ||
            !value->data.pointer.storage->data.is_array()) {
            return std::unexpected(ConstexprError(loc, "unsupported constexpr pointer result"));
        }
        const PointerValue& pointer = value->data.pointer;
        const ArrayValue& array = pointer.storage->data.array;
        snapshot.kind = ConstexprValueKind::StringLiteralPointer;
        // std::max takes both operands by const reference, and scpp will
        // not let a reference borrow a temporary, so the zero has to be a
        // named local.
        std::int64_t zero_index = 0;
        std::size_t first_index = static_cast<std::size_t>(std::max(pointer.index, zero_index));
        for (std::size_t i = first_index; i < array.elements.size(); ++i) {
            std::int64_t ch = array.elements[i]->data.int_value;
            if (ch == 0) break;
            snapshot.string_value.push_back(static_cast<char>(ch));
        }
        return snapshot;
    }
    if (value->data.is_object()) {
        snapshot.kind = ConstexprValueKind::Object;
        // Fields are now snapshotted in the order they were created (i.e. the
        // order they are declared in the struct/class) rather than in
        // unordered_map hash order, so a given object always produces the same
        // object_fields sequence. codegen's only consumer looks fields up by
        // name, so this is a latent-hazard fix, not an output change.
        for (const ObjectField& field : value->data.object.fields) {
            auto field_result = snapshot_constexpr_value(field.cell, loc);
            if (!field_result.has_value()) return std::unexpected(std::move(field_result).error());
            // scpp's overload resolution is exact-match only, so the
            // element is constructed explicitly rather than forwarded
            // through emplace_back's argument pack.
            ConstexprField snapshot_field{field.name,
                                          std::make_shared<ConstexprValue>(std::move(field_result).value())};
            snapshot.object_fields.push_back(std::move(snapshot_field));
        }
        return snapshot;
    }
    if (value->data.is_array()) {
        snapshot.kind = ConstexprValueKind::Array;
        for (const std::shared_ptr<Cell>& element : value->data.array.elements) {
            auto element_result = snapshot_constexpr_value(element, loc);
            if (!element_result.has_value()) return std::unexpected(std::move(element_result).error());
            snapshot.elements.push_back(std::move(element_result).value());
        }
        return snapshot;
    }
    return std::unexpected(ConstexprError(loc, "unsupported constexpr value kind"));
}

[[nodiscard]] std::expected<void, ConstexprError> collect_runtime_expr_rewrites(const Program& program, Expr& expr, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<std::reference_wrapper<Stmt>>& consteval_if_rewrites);
[[nodiscard]] std::expected<void, ConstexprError> collect_runtime_stmt_rewrites(const Program& program, Stmt& stmt, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<std::reference_wrapper<Stmt>>& consteval_if_rewrites);

[[nodiscard]] std::expected<void, ConstexprError> collect_runtime_stmt_rewrites(const Program& program, Stmt& stmt, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<std::reference_wrapper<Stmt>>& consteval_if_rewrites) {
    if (stmt.kind == StmtKind::If && stmt.if_mode != IfMode::Runtime) {
        std::optional<std::reference_wrapper<Stmt>> runtime_branch{};
        if (stmt.if_mode == IfMode::ConstevalFalse) {
            if (stmt.then_branch) runtime_branch = std::optional<std::reference_wrapper<Stmt>>{std::reference_wrapper<Stmt>{*stmt.then_branch}};
        } else if (stmt.else_branch) {
            runtime_branch = std::optional<std::reference_wrapper<Stmt>>{std::reference_wrapper<Stmt>{*stmt.else_branch}};
        }
        if (runtime_branch.has_value()) {
            if (auto result = collect_runtime_stmt_rewrites(program, runtime_branch->get(), engine, expr_rewrites, consteval_if_rewrites);
                !result.has_value()) {
                return result;
            }
        }
        consteval_if_rewrites.push_back(std::reference_wrapper<Stmt>{stmt});
        return {};
    }

    if (stmt.init) {
        if (auto result = collect_runtime_expr_rewrites(program, *stmt.init, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    for (ExprPtr& arg : stmt.ctor_args) {
        if (auto result = collect_runtime_expr_rewrites(program, *arg, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (stmt.expr) {
        if (auto result = collect_runtime_expr_rewrites(program, *stmt.expr, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (stmt.condition) {
        if (auto result = collect_runtime_expr_rewrites(program, *stmt.condition, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (stmt.then_branch) {
        if (auto result = collect_runtime_stmt_rewrites(program, *stmt.then_branch, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (stmt.else_branch) {
        if (auto result = collect_runtime_stmt_rewrites(program, *stmt.else_branch, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    for (StmtPtr& nested : stmt.statements) {
        if (auto result = collect_runtime_stmt_rewrites(program, *nested, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    return {};
}

[[nodiscard]] std::expected<void, ConstexprError> collect_runtime_expr_rewrites(const Program& program, Expr& expr, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<std::reference_wrapper<Stmt>>& consteval_if_rewrites) {
    if (!expr_depends_on_runtime_bindings(expr) && engine.call_names_immediate_function(expr)) {
        auto value_result = engine.evaluate_root_expr(expr);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        expr_rewrites.push_back(ExprRewrite{expr, std::move(value_result).value()});
        return {};
    }
    if (expr.lhs) {
        if (auto result = collect_runtime_expr_rewrites(program, *expr.lhs, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (expr.rhs) {
        if (auto result = collect_runtime_expr_rewrites(program, *expr.rhs, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (expr.third) {
        if (auto result = collect_runtime_expr_rewrites(program, *expr.third, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    for (ExprPtr& arg : expr.args) {
        if (auto result = collect_runtime_expr_rewrites(program, *arg, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    if (expr.lambda_body) {
        if (auto result = collect_runtime_stmt_rewrites(program, *expr.lambda_body, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    return {};
}

void rewrite_consteval_if_for_runtime(Stmt& stmt) {
    if (stmt.kind != StmtKind::If || stmt.if_mode == IfMode::Runtime) return;
    SourceLocation loc = stmt.loc;
    IfMode mode = stmt.if_mode;
    StmtPtr selected{};
    if (mode == IfMode::ConstevalFalse) {
        selected = std::move(stmt.then_branch);
    } else {
        selected = std::move(stmt.else_branch);
    }
    if (selected) {
        stmt = std::move(*selected);
        return;
    }
    stmt = Stmt{};
    stmt.kind = StmtKind::Block;
    stmt.loc = loc;
}

class AlignmentResolver {
public:
    virtual ~AlignmentResolver() = default;
    AlignmentResolver(Program& program, ConstexprEngine& engine) : program_{program}, engine_{engine} {
        // ch05 §9.4/§9.3: an uninstantiated generic struct/class template
        // may freely mention its own not-yet-substituted type parameter
        // inside an array bound or `alignas` operand (e.g. `sizeof(T)`)
        // -- exactly like real C++, that value-dependent expression is
        // only evaluated later, once per concrete instantiation (see
        // monomorphize.cppm), never on the template primary itself. This
        // set names every such primary's own struct/class/method so
        // resolve_struct/resolve_class/run() can skip it entirely below
        // -- mirrors codegen/orchestration.cppm's own
        // generic_type_template_names/is_never_compiled, built for the
        // same reason.
        for (const StructDef& def : program_.structs) {
            if (!def.template_params.empty()) generic_template_owner_names_.insert(def.name);
        }
        for (const ClassDef& def : program_.classes) {
            if (!def.template_params.empty()) generic_template_owner_names_.insert(def.name);
        }
    }

    // ch05 §9.4: resolves every array bound reachable anywhere in the
    // program -- struct/class fields, globals, function parameter/return
    // types, and every local variable inside every function body
    // (including nested blocks/if/while and lambda bodies) -- and
    // deliberately does NOT touch `alignas` at all (that remains `run()`'s
    // job below, unchanged). MUST run before ANY other constant-expression
    // evaluation elsewhere in the pipeline that might query a type's
    // layout (`sizeof`/`alignof`) or execute a function body (which could
    // declare a local array): in particular, this needs to run before
    // `collect_runtime_stmt_rewrites`'s own folding of immediately-invoked
    // consteval calls (see fold_immediate_calls's call site), since that
    // step can execute an arbitrary consteval function body -- including
    // one that declares and subscripts a local array -- long before
    // `run()` below would otherwise get around to it. `run()` still
    // resolves array bounds too (via `resolve_type_dependencies`), but by
    // the time it runs every `array_size_expr` here has already been
    // cleared, so that part of `run()` becomes a harmless no-op safety
    // net for whichever call site is reached first.
    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds() {
        for (StructDef& def : program_.structs) {
            if (auto result = resolve_struct_array_bounds(def.name); !result.has_value()) return result;
        }
        for (ClassDef& def : program_.classes) {
            if (auto result = resolve_class_array_bounds(def.name); !result.has_value()) return result;
        }
        for (GlobalVar& global : program_.globals) {
            if (global.decl == nullptr) continue;
            if (auto result = resolve_array_bounds_type_dependencies(global.decl->type, global.namespace_path); !result.has_value()) return result;
        }
        for (Function& fn : program_.functions) {
            if (fn.is_generic_template) continue;
            if (!fn.member_owner_class.empty() && generic_template_owner_names_.contains(fn.member_owner_class)) {
                continue;
            }
            for (Param& param : fn.params) {
                if (auto result = resolve_array_bounds_type_dependencies(param.type, fn.namespace_path); !result.has_value()) return result;
            }
            if (auto result = resolve_array_bounds_type_dependencies(fn.return_type, fn.namespace_path); !result.has_value()) return result;
            if (fn.body) {
                // ch05 §9.4 (local-constexpr-as-array-bound gap fix):
                // opens this function's own local constant-evaluation
                // frame (see ConstexprEngine::begin_local_array_bound_scope)
                // so a local `constexpr`/const-with-init declaration seen
                // while walking this body below becomes visible to a
                // later array bound in the same function, exactly like it
                // already is for `alignas` via validate_constexpr_locals.
                // Reset fresh per function -- never leaks across functions.
                engine_.begin_local_array_bound_scope(fn.namespace_path);
                auto result = resolve_array_bounds_in_stmt(*fn.body);
                engine_.end_local_array_bound_scope();
                if (!result.has_value()) return result;
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> run() {
        for (StructDef& def : program_.structs) {
            if (auto result = resolve_struct(def.name); !result.has_value()) return result;
        }
        for (ClassDef& def : program_.classes) {
            if (auto result = resolve_class(def.name); !result.has_value()) return result;
        }
        for (GlobalVar& global : program_.globals) {
            if (global.decl == nullptr) continue;
            if (auto result = resolve_type_dependencies(global.decl->type, global.namespace_path); !result.has_value()) return result;
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, global.decl->type);
            if (!layout.has_value()) {
                global.decl->resolved_alignment = 0;
                continue;
            }
            std::string alignment_context{};
            alignment_context += "variable '";
            alignment_context += global.decl->var_name;
            alignment_context += "'";
            auto alignment_result =
                engine_.resolve_root_alignment_specs(global.decl->alignment_specs, layout->abi_align_bytes, alignment_context,
                                                     global.namespace_path);
            if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
            global.decl->resolved_alignment = alignment_result.value();
        }
        // Separate pass, deliberately after every global's *type* is
        // resolved above: one global's initializer may ask for another
        // global's layout (`sizeof`), and that must not depend on
        // declaration order.
        for (GlobalVar& global : program_.globals) {
            if (global.decl == nullptr) continue;
            if (auto result = engine_.validate_constexpr_global(global); !result.has_value()) return result;
        }
        for (Function& fn : program_.functions) {
            if (!fn.body) continue;
            if (fn.is_generic_template) continue;
            if (!fn.member_owner_class.empty() && generic_template_owner_names_.contains(fn.member_owner_class)) {
                continue;
            }
            if (auto result = engine_.validate_constexpr_locals(fn); !result.has_value()) return result;
        }
        return {};
    }

private:
    Program& program_;
    ConstexprEngine& engine_;
    std::unordered_set<std::string> resolving_structs_{};
    std::unordered_set<std::string> resolved_structs_{};
    std::unordered_set<std::string> resolving_classes_{};
    std::unordered_set<std::string> resolved_classes_{};
    std::unordered_set<std::string> generic_template_owner_names_{};
    // ch05 §9.4: separate tracking sets for `resolve_array_bounds()`'s own
    // early pass above -- deliberately NOT shared with
    // resolving_structs_/resolved_structs_ (which mean "fully resolved,
    // alignas included" for `run()`'s later, different pass). Reusing
    // those would make `run()` wrongly skip a struct/class's alignas work
    // entirely, believing it already fully resolved.
    std::unordered_set<std::string> array_bounds_resolving_structs_{};
    std::unordered_set<std::string> array_bounds_resolved_structs_{};
    std::unordered_set<std::string> array_bounds_resolving_classes_{};
    std::unordered_set<std::string> array_bounds_resolved_classes_{};

    // Named-type-aware recursive Type walker used only by
    // resolve_array_bounds() above -- mirrors resolve_type_dependencies
    // below but resolves ONLY array bounds (no alignas/layout work), and
    // additionally recurses into a Named struct/class reference so a
    // `sizeof(Other)` inside an array-bound expression sees Other's own
    // array-sized fields correctly, regardless of declaration order.
    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds_type_dependencies(Type& type,
                                                                                             const std::vector<std::string>& namespace_path) {
        switch (type.kind) {
            case TypeKind::Named:
                if (OptionalStructDefRef bounds_struct = find_struct_mut(type.name); bounds_struct.has_value()) {
                    return resolve_struct_array_bounds(bounds_struct->get().name);
                } else if (OptionalClassDefRef bounds_class = find_class_mut(type.name); bounds_class.has_value()) {
                    return resolve_class_array_bounds(bounds_class->get().name);
                }
                return {};
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) return resolve_array_bounds_type_dependencies(*type.pointee, namespace_path);
                return {};
            case TypeKind::Array:
                if (type.element) {
                    if (auto result = resolve_array_bounds_type_dependencies(*type.element, namespace_path); !result.has_value()) return result;
                }
                if (type.array_size_expr) {
                    auto bound_result = engine_.resolve_root_array_bound(*type.array_size_expr, namespace_path);
                    if (!bound_result.has_value()) return std::unexpected(std::move(bound_result).error());
                    type.array_size = bound_result.value();
                    type.array_size_expr.reset();
                }
                return {};
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) {
                    if (auto result = resolve_array_bounds_type_dependencies(*type.function_return, namespace_path); !result.has_value()) {
                        return result;
                    }
                }
                for (Type& param : type.function_params) {
                    if (auto result = resolve_array_bounds_type_dependencies(param, namespace_path); !result.has_value()) return result;
                }
                return {};
        }
        return {};
    }

    // Full statement-tree walk covering every StmtKind that can carry a
    // nested VarDecl or expression -- mirrors collect_runtime_stmt_rewrites/
    // collect_runtime_expr_rewrites's own traversal shape (the two
    // functions this pass exists specifically to run ahead of). Also
    // mirrors validate_constexpr_stmt_tree's own frame-scoping shape (see
    // ConstexprEngine::push_local_array_bound_scope's comment): a nested
    // frame per Block/If-branch/While-body keeps a local constexpr's
    // visibility scoped exactly like ordinary C++ block scoping.
    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds_in_stmt(Stmt& stmt) {
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                // ch05 §9.4 (local-constexpr-as-array-bound gap fix): uses
                // ConstexprEngine::resolve_array_bounds_in_type (NOT this
                // class's own resolve_array_bounds_type_dependencies)
                // because that engine method resolves the bound in place
                // inside the already-active frame opened by
                // resolve_array_bounds() above, instead of unconditionally
                // clearing it the way resolve_root_array_bound (used by
                // resolve_array_bounds_type_dependencies) does -- clearing
                // here would wipe out every local constexpr bound by an
                // earlier sibling statement in this same function right
                // before it's needed. Recursing into a Named struct/class
                // is unnecessary here: every struct/class is already fully
                // array-bounds-resolved (see resolve_array_bounds()'s own
                // struct/class loops, which always run first).
                if (auto result = engine_.resolve_array_bounds_in_type(stmt.type); !result.has_value()) return result;
                if (stmt.init) {
                    if (auto result = resolve_array_bounds_in_expr(*stmt.init); !result.has_value()) return result;
                }
                for (ExprPtr& arg : stmt.ctor_args) {
                    if (auto result = resolve_array_bounds_in_expr(*arg); !result.has_value()) return result;
                }
                // Makes this local's own value visible to a later
                // sibling/nested array-bound expression in this same
                // function, exactly like it's already visible as an
                // `alignas` operand.
                return engine_.bind_local_constant_for_array_bounds(stmt);
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) return resolve_array_bounds_in_expr(*stmt.expr);
                return {};
            case StmtKind::If:
                if (stmt.condition) {
                    if (auto result = resolve_array_bounds_in_expr(*stmt.condition); !result.has_value()) return result;
                }
                if (stmt.then_branch) {
                    engine_.push_local_array_bound_scope();
                    auto result = resolve_array_bounds_in_stmt(*stmt.then_branch);
                    engine_.pop_local_array_bound_scope();
                    if (!result.has_value()) return result;
                }
                if (stmt.else_branch) {
                    engine_.push_local_array_bound_scope();
                    auto result = resolve_array_bounds_in_stmt(*stmt.else_branch);
                    engine_.pop_local_array_bound_scope();
                    if (!result.has_value()) return result;
                }
                return {};
            case StmtKind::While:
                if (stmt.condition) {
                    if (auto result = resolve_array_bounds_in_expr(*stmt.condition); !result.has_value()) return result;
                }
                if (stmt.then_branch) {
                    engine_.push_local_array_bound_scope();
                    auto result = resolve_array_bounds_in_stmt(*stmt.then_branch);
                    engine_.pop_local_array_bound_scope();
                    if (!result.has_value()) return result;
                }
                return {};
            case StmtKind::Switch: {
                if (stmt.condition) {
                    if (auto result = resolve_array_bounds_in_expr(*stmt.condition); !result.has_value()) return result;
                }
                for (SwitchCase& switch_case : stmt.switch_cases) {
                    if (switch_case.value) {
                        if (auto result = resolve_array_bounds_in_expr(*switch_case.value); !result.has_value()) return result;
                    }
                    engine_.push_local_array_bound_scope();
                    std::expected<void, ConstexprError> result{};
                    for (StmtPtr& nested : switch_case.statements) {
                        result = resolve_array_bounds_in_stmt(*nested);
                        if (!result.has_value()) break;
                    }
                    engine_.pop_local_array_bound_scope();
                    if (!result.has_value()) return result;
                }
                return {};
            }
            case StmtKind::Block: {
                engine_.push_local_array_bound_scope();
                std::expected<void, ConstexprError> result{};
                for (StmtPtr& nested : stmt.statements) {
                    result = resolve_array_bounds_in_stmt(*nested);
                    if (!result.has_value()) break;
                }
                engine_.pop_local_array_bound_scope();
                return result;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Fallthrough:
                return {};
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds_in_expr(Expr& expr) {
        if (expr.lhs) {
            if (auto result = resolve_array_bounds_in_expr(*expr.lhs); !result.has_value()) return result;
        }
        if (expr.rhs) {
            if (auto result = resolve_array_bounds_in_expr(*expr.rhs); !result.has_value()) return result;
        }
        if (expr.third) {
            if (auto result = resolve_array_bounds_in_expr(*expr.third); !result.has_value()) return result;
        }
        for (ExprPtr& arg : expr.args) {
            if (auto result = resolve_array_bounds_in_expr(*arg); !result.has_value()) return result;
        }
        if (expr.lambda_body) return resolve_array_bounds_in_stmt(*expr.lambda_body);
        return {};
    }

    // Array-bounds-only counterpart of resolve_struct/resolve_class below
    // (no alignas work, and using this class's own separate
    // array_bounds_resolving_*/array_bounds_resolved_* sets).
    [[nodiscard]] std::expected<void, ConstexprError> resolve_struct_array_bounds(const std::string& name) {
        if (array_bounds_resolved_structs_.contains(name)) return {};
        if (!array_bounds_resolving_structs_.insert(name).second) return {};
        OptionalStructDefRef struct_ref = find_struct_mut(name);
        if (!struct_ref.has_value()) return {};
        StructDef& def = struct_ref->get();
        if (!def.template_params.empty()) {
            array_bounds_resolving_structs_.erase(name);
            array_bounds_resolved_structs_.insert(name);
            return {};
        }
        engine_.mark_type_incomplete(name);
        for (StructField& field : def.fields) {
            if (auto result = resolve_array_bounds_type_dependencies(field.type, def.namespace_path); !result.has_value()) return result;
        }
        engine_.mark_type_complete(name);
        array_bounds_resolving_structs_.erase(name);
        array_bounds_resolved_structs_.insert(name);
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> resolve_class_array_bounds(const std::string& name) {
        if (array_bounds_resolved_classes_.contains(name)) return {};
        if (!array_bounds_resolving_classes_.insert(name).second) return {};
        OptionalClassDefRef class_ref = find_class_mut(name);
        if (!class_ref.has_value()) return {};
        ClassDef& def = class_ref->get();
        if (!def.template_params.empty()) {
            array_bounds_resolving_classes_.erase(name);
            array_bounds_resolved_classes_.insert(name);
            return {};
        }
        // ch05 §5.14: a "checking class" (ClassDef::is_synthetic_check_only,
        // see check_generic_type_methods_once) is a purely internal,
        // witness-substituted artifact synthesized only so movecheck can
        // check one generic method's body once, abstractly -- its fields
        // (including any array-bound expression like `sizeof(T)` with `T`
        // replaced by the zero-field bare-witness struct) are never real
        // storage and must never be evaluated/validated here: doing so
        // would spuriously reject an entirely legitimate template-
        // parameter-dependent bound (e.g. `char storage[sizeof(T)]` in a
        // bare, unconstrained `class Box<T>`) at the generic-definition
        // site, long before real instantiation ever substitutes a real
        // concrete type for T (see also codegen/orchestration.cppm's own
        // `if (def.is_synthetic_check_only) continue;`, the established
        // precedent for skipping these synthetic classes entirely).
        if (def.is_synthetic_check_only) {
            array_bounds_resolving_classes_.erase(name);
            array_bounds_resolved_classes_.insert(name);
            return {};
        }
        engine_.mark_type_incomplete(name);
        if (auto base = def.direct_ordinary_base(); base.has_value()) {
            Type base_type = base->get().base_type;
            if (auto result = resolve_array_bounds_type_dependencies(base_type, def.namespace_path); !result.has_value()) return result;
        }
        for (ClassField& field : def.fields) {
            if (auto result = resolve_array_bounds_type_dependencies(field.type, def.namespace_path); !result.has_value()) return result;
        }
        engine_.mark_type_complete(name);
        array_bounds_resolving_classes_.erase(name);
        array_bounds_resolved_classes_.insert(name);
        return {};
    }

    [[nodiscard]] OptionalStructDefRef find_struct_mut(const std::string& name) {
        for (StructDef& def : program_.structs) {
            if (def.name == name) return make_struct_def_ref(def);
        }
        return {};
    }

    [[nodiscard]] OptionalClassDefRef find_class_mut(const std::string& name) {
        for (ClassDef& def : program_.classes) {
            if (def.name == name) return make_class_def_ref(def);
        }
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> resolve_type_dependencies(Type& type,
                                                                                const std::vector<std::string>& namespace_path) {
        switch (type.kind) {
            case TypeKind::Named:
                if (OptionalStructDefRef dep_struct = find_struct_mut(type.name); dep_struct.has_value()) {
                    return resolve_struct(dep_struct->get().name);
                } else if (OptionalClassDefRef dep_class = find_class_mut(type.name); dep_class.has_value()) {
                    return resolve_class(dep_class->get().name);
                }
                return {};
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) return resolve_type_dependencies(*type.pointee, namespace_path);
                return {};
            case TypeKind::Array:
                if (type.element) {
                    if (auto result = resolve_type_dependencies(*type.element, namespace_path); !result.has_value()) return result;
                }
                // ch05 §9.4: this array's own bound (not its element's --
                // that was just handled by the recursive call above)
                // must be resolved before any layout_of_type call below
                // (natural_field_alignment/natural_struct_alignment/
                // natural_class_alignment, and this same function's own
                // callers) ever reads `array_size`.
                if (type.array_size_expr) {
                    auto bound_result = engine_.resolve_root_array_bound(*type.array_size_expr, namespace_path);
                    if (!bound_result.has_value()) return std::unexpected(std::move(bound_result).error());
                    type.array_size = bound_result.value();
                    type.array_size_expr.reset();
                }
                return {};
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) {
                    if (auto result = resolve_type_dependencies(*type.function_return, namespace_path); !result.has_value()) return result;
                }
                for (Type& param : type.function_params) {
                    if (auto result = resolve_type_dependencies(param, namespace_path); !result.has_value()) return result;
                }
                return {};
        }
        return {};
    }

    [[nodiscard]] bool type_has_strengthened_record_alignment(const Type& type) {
        if (type.kind == TypeKind::Array && type.element) return type_has_strengthened_record_alignment(*type.element);
        if (type.kind != TypeKind::Named) return false;
        if (OptionalStructDefRef aligned_struct = find_struct_mut(type.name); aligned_struct.has_value()) {
            return aligned_struct->get().resolved_alignment != 0;
        }
        if (OptionalClassDefRef aligned_class = find_class_mut(type.name); aligned_class.has_value()) {
            return aligned_class->get().resolved_alignment != 0;
        }
        return false;
    }

    [[nodiscard]] std::optional<std::uint64_t> natural_field_alignment(const StructField& field) {
        std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
        if (!layout.has_value()) return std::nullopt;
        return std::max(layout->abi_align_bytes, field.resolved_alignment);
    }

    [[nodiscard]] std::optional<std::uint64_t> natural_field_alignment(const ClassField& field) {
        std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
        if (!layout.has_value()) return std::nullopt;
        return std::max(layout->abi_align_bytes, field.resolved_alignment);
    }

    [[nodiscard]] std::uint64_t natural_struct_alignment(const StructDef& def) {
        if (def.is_packed) return 1;
        std::uint64_t overall = 1;
        for (const StructField& field : def.fields) {
            if (std::optional<std::uint64_t> align = natural_field_alignment(field); align.has_value()) {
                overall = std::max(overall, *align);
            }
        }
        return overall;
    }

    [[nodiscard]] std::uint64_t natural_class_alignment(const ClassDef& def) {
        std::uint64_t overall = 1;
        if (auto base = def.direct_ordinary_base(); base.has_value()) {
            std::optional<TypeLayoutInfo> base_layout = layout_of_type(program_, base->get().base_type);
            if (base_layout.has_value()) overall = std::max(overall, base_layout->abi_align_bytes);
        }
        for (const ClassField& field : def.fields) {
            if (std::optional<std::uint64_t> align = natural_field_alignment(field); align.has_value()) {
                overall = std::max(overall, *align);
            }
        }
        return overall;
    }

    [[nodiscard]] std::expected<void, ConstexprError> resolve_struct(const std::string& name) {
        if (resolved_structs_.contains(name)) return {};
        if (!resolving_structs_.insert(name).second) return {};
        OptionalStructDefRef struct_ref = find_struct_mut(name);
        if (!struct_ref.has_value()) return {};
        StructDef& def = struct_ref->get();
        if (!def.template_params.empty()) {
            // ch05 §9.4(7)/§9.3: this is the primary template
            // definition itself, not a concrete instantiation -- its
            // fields may freely reference the template's own,
            // not-yet-substituted type parameter(s) (e.g. `char
            // storage[sizeof(T)];`), which are not real types yet.
            // monomorphize.cppm's instantiate_generic_type clones this
            // struct's fields with each type parameter already
            // substituted for a concrete type; that clone -- an
            // ordinary struct with `template_params` empty -- goes
            // through this same function on its own, separate call,
            // where resolution proceeds normally.
            resolving_structs_.erase(name);
            resolved_structs_.insert(name);
            return {};
        }
        engine_.mark_type_incomplete(name);
        for (StructField& field : def.fields) {
            if (auto result = resolve_type_dependencies(field.type, def.namespace_path); !result.has_value()) return result;
        }
        if (def.is_packed && !def.alignment_specs.empty()) {
            std::string message{};
            message += "'[[scpp::packed]]' cannot be combined with 'alignas' on '";
            message += def.name;
            message += "'";
            return std::unexpected(ConstexprError(def.alignment_specs.front().loc, message));
        }
        for (StructField& field : def.fields) {
            if (def.is_packed && !field.alignment_specs.empty()) {
                std::string message{};
                message += "'[[scpp::packed]]' cannot be combined with 'alignas' on member '";
                message += field.name;
                message += "'";
                return std::unexpected(ConstexprError(field.alignment_specs.front().loc, message));
            }
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
            if (!layout.has_value()) {
                field.resolved_alignment = 0;
                continue;
            }
            std::string alignment_context{};
            alignment_context += "member '";
            alignment_context += field.name;
            alignment_context += "'";
            auto alignment_result =
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, alignment_context,
                                                     def.namespace_path);
            if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
            field.resolved_alignment = alignment_result.value();
            if (def.is_packed && type_has_strengthened_record_alignment(field.type)) {
                std::string message{};
                message += "'[[scpp::packed]]' member '";
                message += field.name;
                message += "' cannot have a class/struct/union type whose alignment was strengthened by 'alignas'";
                return std::unexpected(ConstexprError(field.loc, message));
            }
        }
        std::uint64_t natural_align = natural_struct_alignment(def);
        std::string def_alignment_context{};
        def_alignment_context += std::string(def.is_union ? "union '" : "struct '");
        def_alignment_context += def.name;
        def_alignment_context += "'";
        auto def_alignment_result =
            engine_.resolve_root_alignment_specs(def.alignment_specs, natural_align, def_alignment_context,
                                                 def.namespace_path);
        if (!def_alignment_result.has_value()) return std::unexpected(std::move(def_alignment_result).error());
        def.resolved_alignment = def_alignment_result.value();
        engine_.mark_type_complete(name);
        resolving_structs_.erase(name);
        resolved_structs_.insert(name);
        return {};
    }

    [[nodiscard]] std::expected<void, ConstexprError> resolve_class(const std::string& name) {
        if (resolved_classes_.contains(name)) return {};
        if (!resolving_classes_.insert(name).second) return {};
        OptionalClassDefRef class_ref = find_class_mut(name);
        if (!class_ref.has_value()) return {};
        ClassDef& def = class_ref->get();
        if (!def.template_params.empty()) {
            // See the identical comment in resolve_struct above.
            resolving_classes_.erase(name);
            resolved_classes_.insert(name);
            return {};
        }
        // ch05 §9.4/§5.14: skip a synthetic "checking class"
        // (ClassDef::is_synthetic_check_only) here too, for the same
        // reason as the identical check in resolve_class_array_bounds
        // above -- it is never codegen'd (codegen/orchestration.cppm's
        // own `if (def.is_synthetic_check_only) continue;`) and its
        // resolved_alignment/array bounds are never read by anything, so
        // there is no reason to risk this pass throwing on a witness-
        // substituted field type (e.g. `sizeof(T)` with T replaced by the
        // zero-field bare-witness struct) that was never a real bound to
        // begin with.
        if (def.is_synthetic_check_only) {
            resolving_classes_.erase(name);
            resolved_classes_.insert(name);
            return {};
        }
        engine_.mark_type_incomplete(name);
        if (auto base = def.direct_ordinary_base(); base.has_value()) {
            Type base_type = base->get().base_type;
            if (auto result = resolve_type_dependencies(base_type, def.namespace_path); !result.has_value()) return result;
        }
        for (ClassField& field : def.fields) {
            if (auto result = resolve_type_dependencies(field.type, def.namespace_path); !result.has_value()) return result;
        }
        for (ClassField& field : def.fields) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
            if (!layout.has_value()) {
                field.resolved_alignment = 0;
                continue;
            }
            std::string alignment_context{};
            alignment_context += "member '";
            alignment_context += field.name;
            alignment_context += "'";
            auto alignment_result =
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, alignment_context,
                                                     def.namespace_path);
            if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
            field.resolved_alignment = alignment_result.value();
        }
        std::uint64_t natural_align = natural_class_alignment(def);
        std::string def_alignment_context{};
        def_alignment_context += "class '";
        def_alignment_context += def.name;
        def_alignment_context += "'";
        auto def_alignment_result =
            engine_.resolve_root_alignment_specs(def.alignment_specs, natural_align, def_alignment_context,
                                                 def.namespace_path);
        if (!def_alignment_result.has_value()) return std::unexpected(std::move(def_alignment_result).error());
        def.resolved_alignment = def_alignment_result.value();
        engine_.mark_type_complete(name);
        resolving_classes_.erase(name);
        resolved_classes_.insert(name);
        return {};
    }
};

[[nodiscard]] std::expected<void, ConstexprError> fold_immediate_calls(Program& program, ConstexprLimits limits) {
    ConstexprEngine engine{program, limits};
    AlignmentResolver aligner{program, engine};
    // ch05 §9.4: array bounds must be resolved before *any* immediate-call
    // folding below, since folding a call to a consteval function actually
    // executes its body (via evaluate_root_expr/execute_stmt) -- including
    // any local array declaration inside it -- long before `aligner.run()`
    // would otherwise get around to validating that same function. See
    // resolve_array_bounds()'s own comment for the full rationale.
    if (auto result = aligner.resolve_array_bounds(); !result.has_value()) return result;
    std::vector<ExprRewrite> expr_rewrites{};
    std::vector<std::reference_wrapper<Stmt>> consteval_if_rewrites{};
    for (Function& fn : program.functions) {
        if (!fn.body) continue;
        if (auto result = collect_runtime_stmt_rewrites(program, *fn.body, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    // ch09 §9.1(4): *every* potentially-evaluated call to an immediate
    // function shall produce a constant expression -- a global's
    // initializer is not an exception, and this holds whether or not the
    // global is itself `constexpr`. This loop was simply absent: only
    // function bodies were walked, so a `consteval` call at namespace
    // scope was never folded, survived into codegen, and hit codegen's
    // deliberate "a consteval function is never compiled" skip as
    // `internal error: no generated code for resolved function 'f'`. That
    // made every immediate function uncallable from any namespace-scope
    // initializer.
    for (GlobalVar& global : program.globals) {
        if (global.decl == nullptr || !global.decl->init) continue;
        if (auto result = collect_runtime_expr_rewrites(program, *global.decl->init, engine, expr_rewrites, consteval_if_rewrites);
            !result.has_value()) {
            return result;
        }
    }
    for (ExprRewrite& rewrite : expr_rewrites) {
        // The target Expr lives in `program`, not in `rewrite`, but the
        // borrow checker can only see that both arguments are reached
        // through `rewrite`; copying the (shared) value out first gives it
        // two provably distinct operands.
        std::shared_ptr<Cell> rewrite_value = rewrite.value;
        Expr& rewrite_target = rewrite.target.get();
        if (auto result = rewrite_expr_as_constant(rewrite_target, rewrite_value); !result.has_value()) return result;
    }
    // Validate required-constant-expression contexts *before* stripping
    // `if consteval` / `if !consteval` down to their runtime-selected
    // branches. Otherwise a `constexpr` local initializer that calls a
    // constexpr function containing `if consteval` would incorrectly see
    // only the runtime branch, because the callee's AST has already been
    // destructively rewritten for the later runtime pipeline.
    if (auto result = aligner.run(); !result.has_value()) return result;
    for (std::reference_wrapper<Stmt> stmt : consteval_if_rewrites) rewrite_consteval_if_for_runtime(stmt.get());
    // Consteval bodies are deliberately *kept* here. This pass used to end
    // by discarding them (`fn.body.reset()` for every Consteval function
    // whose name did not end in "_new"), which dated from a time when
    // codegen would otherwise have tried to emit them. Codegen has since
    // grown an explicit, unconditional skip -- see is_never_compiled in
    // codegen/orchestration.cppm, which returns true for every Consteval
    // function -- so the reset no longer protected anything.
    //
    // What it did still do was blind movecheck: compile_program runs this
    // pass immediately before check_moves (src/driver.cppm), so a consteval
    // function reached the borrow/move checker with no body at all and every
    // ch02 rule, plus the body-walking half of spec §11.2(5), silently
    // walked nothing. Constructors escaped only by accident -- the
    // "_new" suffix carve-out was added to make consteval class-argument
    // conversion work, and having them checked was a side effect of a
    // name-shaped stand-in for "is this a constructor".
    //
    // A consteval body is an ordinary function body: ch02's ownership rules
    // describe the program rather than the machine, so a use-after-move
    // during constant evaluation is just as ill-formed as one at run time.
    // (The §5.1(5.1) raw-pointer gates are the documented exception, and
    // they are exempted where they are enforced -- movecheck/dataflow.cppm's
    // entry state -- because ch06 §7.3(1) makes `[[scpp::unsafe]]`
    // unevaluatable during constant evaluation, so the licence they demand
    // cannot be written there. That exemption is what keeps this change and
    // #466 describing one model rather than two.)
    return {};
}

[[nodiscard]] std::expected<ConstexprValue, ConstexprError> evaluate_immediate_expr(const Program& program, const Expr& expr, ConstexprLimits limits) {
    ConstexprEngine engine{program, limits};
    auto value_result = engine.evaluate_root_expr(expr);
    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
    return snapshot_constexpr_value(value_result.value(), expr.loc);
}

} // namespace scpp
