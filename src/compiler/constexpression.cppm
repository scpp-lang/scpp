module;

export module scpp.constexpression;

import std;
import scpp.ast;

export namespace scpp {

struct ConstexprLimits {
    int max_steps = 1000000;
    // Lowered from 512: propagating std::expected<T, ConstexprError>
    // through this engine's mutually-recursive evaluation walk (call_function
    // -> execute_stmt -> evaluate_expr -> evaluate_call_expr -> ...) costs
    // more C++ stack per level than the exceptions this engine used to
    // throw, since every frame now materializes its own expected<T, E>
    // return value instead of unwinding past it. 512 levels of recursion
    // reliably overflowed an 8 MiB stack (a Debug build's default) before
    // this engine's own budget check could ever fire; 256 leaves a wide,
    // empirically-verified safety margin (the crash threshold measured
    // well above 300) and is still far deeper than any real constexpr/
    // consteval recursion is likely to need.
    int max_recursion_depth = 256;
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

[[nodiscard]] bool types_equal(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
    if (a.is_const_qualified != b.is_const_qualified) return false;
    switch (a.kind) {
        case TypeKind::Named:
            if (a.name != b.name || a.template_args.size() != b.template_args.size() ||
                a.non_type_args.size() != b.non_type_args.size()) {
                return false;
            }
            for (std::size_t i = 0; i < a.template_args.size(); ++i) {
                if (!types_equal(a.template_args[i], b.template_args[i])) return false;
            }
            return true;
        case TypeKind::Pointer:
            return a.is_mutable_pointee == b.is_mutable_pointee && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Reference:
            return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Array:
            return a.array_size == b.array_size && a.element && b.element && types_equal(*a.element, *b.element);
        case TypeKind::Span:
            return a.is_mutable_ref == b.is_mutable_ref && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Function:
        case TypeKind::FunctionPointer:
            if (a.function_params.size() != b.function_params.size() || !a.function_return || !b.function_return ||
                !types_equal(*a.function_return, *b.function_return)) {
                return false;
            }
            for (std::size_t i = 0; i < a.function_params.size(); ++i) {
                if (!types_equal(a.function_params[i], b.function_params[i])) return false;
            }
            return a.is_const_function == b.is_const_function &&
                   a.function_ref_qualifier == b.function_ref_qualifier &&
                   a.is_unsafe_function_pointer == b.is_unsafe_function_pointer;
    }
    return false;
}

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
[[nodiscard]] bool is_integral_named_type(const std::string& name) {
    return name == "int" || name == "bool" || name == "char" || name == "long" || name == "unsigned int" ||
           name == "unsigned long" || name == "size_t" || name == "ptrdiff_t" || name == "int8_t" ||
           name == "int16_t" || name == "int32_t" || name == "int64_t" || name == "uint8_t" ||
           name == "uint16_t" || name == "uint32_t" || name == "uint64_t";
}

[[nodiscard]] bool is_integer_like(const Type& type) {
    return type.kind == TypeKind::Named && is_integral_named_type(type.name);
}

[[nodiscard]] bool is_floating_like(const Type& type) {
    return type.kind == TypeKind::Named &&
           (type.name == "float" || type.name == "double" || type.name == "float32_t" || type.name == "float64_t");
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
        for (std::size_t i = 0; i < program_.functions.size(); ++i) functions_by_name_[program_.functions[i].name].push_back(i);
        for (const ClassDef& def : program_.classes) classes_by_name_.emplace(def.name, &def);
        for (const StructDef& def : program_.structs) structs_by_name_.emplace(def.name, &def);
        for (const GlobalVar& global : program_.globals) {
            if (global.decl != nullptr) globals_by_name_.emplace(global.decl->var_name, &global);
        }
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_root_expr(const Expr& expr) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return evaluate_expr(expr);
    }

    [[nodiscard]] std::expected<void, ConstexprError> validate_constexpr_locals(Function& fn) {
        if (!fn.body) return {};
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        frames_.emplace_back();
        auto result = validate_constexpr_stmt_tree(*fn.body);
        frames_.pop_back();
        return result;
    }

    [[nodiscard]] std::expected<std::uint64_t, ConstexprError> resolve_root_alignment_specs(const std::vector<AlignmentSpecifier>& specs, std::uint64_t natural_alignment,
                                               const SourceLocation& loc, const std::string& what) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return resolve_alignment_specs(specs, natural_alignment, loc, what);
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
    [[nodiscard]] std::expected<std::int64_t, ConstexprError> resolve_root_array_bound(const Expr& expr) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return evaluate_and_validate_array_bound(expr);
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
    void begin_local_array_bound_scope() {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        frames_.emplace_back();
    }

    void end_local_array_bound_scope() { frames_.pop_back(); }

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
            (void)execute_stmt(stmt, named_type("void"));
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
    int string_storage_counter_ = 0;
    std::vector<std::unordered_map<std::string, Binding>> frames_{};
    std::unordered_map<std::string, std::vector<std::size_t>> functions_by_name_{};
    std::unordered_map<std::string, const ClassDef*> classes_by_name_{};
    std::unordered_map<std::string, const StructDef*> structs_by_name_{};
    std::unordered_set<std::string> incomplete_type_names_{};
    // ch05 §9.4(8)/06-constant-evaluation.md: a required constant
    // expression (an array bound, an `alignas` operand, ...) may name a
    // global `constexpr` variable (e.g. `constexpr int kBufferSize = 64;
    // char buf[kBufferSize];`) -- unlike an ordinary local, a global is
    // never pushed onto frames_ by any statement-execution path, so
    // lookup_binding falls back to these when a plain frame-stack lookup
    // finds nothing. globals_by_name_ indexes every global for that
    // fallback; resolved_global_constants_ memoizes each global's own
    // once-evaluated value (a global constexpr initializer is evaluated
    // at most once, no matter how many other constant expressions go on
    // to reference it); globals_resolving_ detects `constexpr int A =
    // B; constexpr int B = A;`-style circular dependencies instead of
    // recursing forever.
    std::unordered_map<std::string, const GlobalVar*> globals_by_name_{};
    std::unordered_map<std::string, std::shared_ptr<Cell>> resolved_global_constants_{};
    std::unordered_set<std::string> globals_resolving_{};

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
                                                        std::uint64_t natural_alignment, const SourceLocation& loc,
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
                message += std::to_string(requested);
                message += ", which is less strict than the natural alignment ";
                message += std::to_string(natural_alignment);
                message += " of ";
                message += what;
                return std::unexpected(ConstexprError(spec.loc, message));
            }
            strictest = std::max(strictest, requested);
        }
        static_cast<void>(loc);
        return strictest > natural_alignment ? strictest : 0;
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
                        resolve_alignment_specs(stmt.alignment_specs, layout->abi_align_bytes, stmt.loc,
                                                alignment_context);
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
                        (void)rewrite_expr_as_constant(*stmt.init, binding_result.value().cell);
                    }
                } else if (stmt.is_const && (stmt.init || stmt.has_ctor_args)) {
                    (void)execute_stmt(stmt, named_type("void"));
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
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("double");
        cell->data.set_double(value);
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_bool_cell(bool value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("bool");
        cell->data.set_bool(value);
        return cell;
    }

    [[nodiscard]] std::expected<std::vector<ClassField>, ConstexprError> collect_class_fields(const ClassDef& def) {
        std::vector<ClassField> fields{};
        if (auto base = def.direct_ordinary_base(); base.has_value()) {
            auto base_it = classes_by_name_.find(base->get().base_type.name);
            if (base_it == classes_by_name_.end()) {
                std::string message{};
                message += "missing constexpr class definition for base class '";
                message += base->get().base_type.name;
                message += "'";
                return std::unexpected(ConstexprError(SourceLocation{}, message));
            }
            auto base_fields_result = collect_class_fields(*base_it->second);
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
                if (auto struct_it = structs_by_name_.find(type.name); struct_it != structs_by_name_.end()) {
                    ObjectValue object{};
                    object.type_name = type.name;
                    for (const StructField& field : struct_it->second->fields) {
                        auto field_result = make_default_cell(field.type, loc);
                        if (!field_result.has_value()) return std::unexpected(std::move(field_result).error());
                        object.add_field(field.name, std::move(field_result).value());
                    }
                    cell->data.set_object(std::move(object));
                    return cell;
                }
                if (auto class_it = classes_by_name_.find(type.name); class_it != classes_by_name_.end()) {
                    ObjectValue object{};
                    object.type_name = type.name;
                    auto fields_result = collect_class_fields(*class_it->second);
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
                message += "' is not constexpr-compatible in Phase D1";
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
                return std::unexpected(ConstexprError(loc, "type is not yet supported by the constexpr evaluator in Phase D1"));
            case TypeKind::Span:
                if (type.is_mutable_ref) {
                    return std::unexpected(ConstexprError(loc, "mutable std::span<T> is not supported during constant evaluation"));
                }
                cell->data.set_span(SpanValue{});
                return cell;
        }
        return std::unexpected(ConstexprError(loc, "unsupported constexpr type"));
    }

    [[nodiscard]] std::expected<Binding, ConstexprError> lookup_binding(const std::string& name, const SourceLocation& loc) {
        // std::vector has no rbegin()/rend() yet -- walk backwards (innermost
        // frame first) by index instead, as parser.cppm already does.
        for (std::size_t i = frames_.size(); i > 0; --i) {
            const std::unordered_map<std::string, Binding>& frame = frames_[i - 1];
            auto binding_it = frame.find(name);
            if (binding_it != frame.end()) return binding_it->second;
        }
        auto global_result = resolve_global_constant(name, loc);
        if (!global_result.has_value()) return std::unexpected(std::move(global_result).error());
        if (std::shared_ptr<Cell> global_value = std::move(global_result).value(); global_value != nullptr) {
            return Binding{global_value, /*read_only=*/true};
        }
        std::string message{};
        message += "expression is not a constant expression: identifier '";
        message += name;
        message += "' is not available";
        return std::unexpected(ConstexprError(loc, message));
    }

    // ch05 §9.4(8): a required constant expression may name a global
    // `constexpr` variable (e.g. `constexpr int kBufferSize = 64; char
    // buf[kBufferSize];`, straight from the spec's own accepted-examples
    // list) -- returns nullptr for any name that isn't such a global (a
    // plain runtime global, or no global at all), so lookup_binding's
    // existing "identifier is not available" diagnostic still fires for
    // those. Evaluates the global's own initializer, at most once
    // (memoized in resolved_global_constants_), in a completely isolated
    // frame stack: a global initializer must only ever see other
    // globals/functions, never whatever local variables happen to be
    // live in the caller that triggered this lookup.
    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> resolve_global_constant(const std::string& name, const SourceLocation& loc) {
        if (auto cached = resolved_global_constants_.find(name); cached != resolved_global_constants_.end()) {
            return cached->second;
        }
        auto global_it = globals_by_name_.find(name);
        if (global_it == globals_by_name_.end()) return nullptr;
        const GlobalVar& global = *global_it->second;
        if (global.decl == nullptr || !global.decl->is_constexpr || !global.decl->init) return nullptr;
        if (globals_resolving_.contains(name)) {
            std::string message{};
            message += "constant expression circularly depends on global constexpr variable '";
            message += name;
            message += "'";
            return std::unexpected(ConstexprError(loc, message));
        }
        globals_resolving_.insert(name);
        std::vector<std::unordered_map<std::string, Binding>> saved_frames = std::move(frames_);
        frames_.clear();
        auto value_result = evaluate_expr(*global.decl->init);
        frames_ = std::move(saved_frames);
        globals_resolving_.erase(name);
        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
        std::shared_ptr<Cell> value = std::move(value_result).value();
        resolved_global_constants_.emplace(name, value);
        return value;
    }

    [[nodiscard]] std::expected<std::int64_t, ConstexprError> as_integer(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
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
        if (is_integer_like(cell->type)) return as_integer(cell, loc);
        if (is_enum_like(cell->type)) return cell->data.int_value;
        return std::unexpected(ConstexprError(loc, "switch requires an integral or enum constexpr value"));
    }

    [[nodiscard]] IntegerBounds integer_bounds_for_type(const Type& type) const {
        if (is_named_type(type, "char")) return IntegerBounds{0, 255};
        if (is_named_type(type, "bool")) return IntegerBounds{0, 1};
        if (is_named_type(type, "int")) {
            return IntegerBounds{int32_min_value, int32_max_value};
        }
        if (is_named_type(type, "int8_t")) return IntegerBounds{-128, 127};
        if (is_named_type(type, "uint8_t")) return IntegerBounds{0, 255};
        if (is_named_type(type, "int16_t")) return IntegerBounds{-32768, 32767};
        if (is_named_type(type, "uint16_t")) return IntegerBounds{0, 65535};
        if (is_named_type(type, "int32_t")) {
            return IntegerBounds{int32_min_value, int32_max_value};
        }
        if (is_named_type(type, "unsigned int")) return IntegerBounds{0, uint32_max_value};
        if (is_named_type(type, "size_t") || is_named_type(type, "uint64_t") || is_named_type(type, "unsigned long")) {
            return IntegerBounds{0, int64_max_value};
        }
        // ptrdiff_t/int64_t/long fall through to the same 64-bit bounds the
        // default returns, so they need no branch of their own.
        return IntegerBounds{int64_min_value, int64_max_value};
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
        if (!types_equal(target->type, source->type)) {
            return std::unexpected(ConstexprError(loc, "constexpr assignment requires exactly matching types"));
        }
        std::shared_ptr<Cell> cloned = clone_cell(source);
        target->data = std::move(cloned->data);
        return {};
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
        span.pointer.storage_id += std::to_string(string_storage_counter_ + 1);
        span.size = static_cast<std::int64_t>(array.elements.size());
        result->data.set_span(std::move(span));
        return result;
    }

    [[nodiscard]] std::expected<LValue, ConstexprError> resolve_lvalue(const Expr& expr) {
        if (auto result = tick(expr.loc, "resolving an lvalue"); !result.has_value()) return std::unexpected(std::move(result).error());
        switch (expr.kind) {
            case ExprKind::Identifier: {
                auto binding_result = lookup_binding(expr.name, expr.loc);
                if (!binding_result.has_value()) return std::unexpected(std::move(binding_result).error());
                Binding binding = std::move(binding_result).value();
                return LValue{binding.cell, binding.read_only};
            }
            case ExprKind::Member: {
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
            }
            case ExprKind::Subscript: {
                std::shared_ptr<Cell> subscript_base{};
                bool base_read_only = false;
                // Speculatively try lvalue resolution first (needed so
                // that e.g. `arr[i] = 1` assigns through the real
                // storage); an expression that isn't itself an lvalue
                // (e.g. a function call returning an array by value)
                // falls back to plain evaluation, matching the original
                // try/catch(const ConstexprError&) fallback exactly.
                auto base_lvalue_result = resolve_lvalue(*expr.lhs);
                if (base_lvalue_result.has_value()) {
                    subscript_base = base_lvalue_result.value().cell;
                    base_read_only = base_lvalue_result.value().read_only;
                } else {
                    auto base_value_result = evaluate_expr(*expr.lhs);
                    if (!base_value_result.has_value()) return std::unexpected(std::move(base_value_result).error());
                    subscript_base = std::move(base_value_result).value();
                }
                auto index_value_result = evaluate_expr(*expr.rhs);
                if (!index_value_result.has_value()) return std::unexpected(std::move(index_value_result).error());
                auto index_result = as_integer(index_value_result.value(), expr.loc);
                if (!index_result.has_value()) return std::unexpected(std::move(index_result).error());
                std::int64_t index = index_result.value();
                if (subscript_base->data.is_array()) {
                    const ArrayValue& array = subscript_base->data.array;
                    if (index < 0 || static_cast<std::size_t>(index) >= array.elements.size()) {
                        return std::unexpected(ConstexprError(expr.loc, "constexpr subscript out of bounds"));
                    }
                    return LValue{array.elements[static_cast<std::size_t>(index)], base_read_only};
                }
                if (subscript_base->data.is_span()) {
                    const SpanValue& span = subscript_base->data.span;
                    if (index < 0 || index >= span.size) {
                        return std::unexpected(ConstexprError(expr.loc, "constexpr span subscript out of bounds"));
                    }
                    PointerValue element_ptr = span.pointer;
                    element_ptr.index += index;
                    auto dereferenced = dereference_pointer(element_ptr, make_pointer_type_to(*subscript_base->type.pointee, false), expr.loc);
                    if (!dereferenced.has_value()) return std::unexpected(std::move(dereferenced).error());
                    return LValue{std::move(dereferenced).value(), true};
                }
                if (subscript_base->data.is_pointer()) {
                    if (!subscript_base->type.pointee) return std::unexpected(ConstexprError(expr.loc, "malformed constexpr pointer type"));
                    PointerValue shifted = subscript_base->data.pointer;
                    shifted.index += index;
                    if (!shifted.storage || !shifted.storage->data.is_array()) {
                        return std::unexpected(ConstexprError(expr.loc, "constexpr pointer does not point to indexable storage"));
                    }
                    if (shifted.index < 0 || static_cast<std::size_t>(shifted.index) >= shifted.storage->data.array.elements.size()) {
                        return std::unexpected(ConstexprError(expr.loc, "constexpr subscript out of bounds"));
                    }
                    auto dereferenced = dereference_pointer(shifted, subscript_base->type, expr.loc);
                    if (!dereferenced.has_value()) return std::unexpected(std::move(dereferenced).error());
                    return LValue{std::move(dereferenced).value(), true};
                }
                return std::unexpected(ConstexprError(expr.loc, "constexpr subscript requires an array, pointer, or std::span"));
            }
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
            // Index loop rather than a range-for, and std::uint8_t rather
            // than the bare `unsigned char` shorthand: scpp's range-for
            // takes a fixed-size array, std::span or std::vector (not a
            // std::string), and requires `unsigned` be followed by
            // `int`/`long` (ch06 §6). The cast through std::uint8_t keeps
            // bytes >= 0x80 positive, exactly as `unsigned char` did.
            std::int64_t ch = static_cast<std::int64_t>(static_cast<std::uint8_t>(expr.name.at(i)));
            array.elements.push_back(make_scalar_cell(named_type("char"), ch));
        }
        array.elements.push_back(make_scalar_cell(named_type("char"), 0));
        storage->data.set_array(std::move(array));

        auto result = std::make_shared<Cell>();
        result->type = make_const_char_pointer_type();
        PointerValue pointer{};
        pointer.storage = storage;
        pointer.storage_id = "string#";
        pointer.storage_id += std::to_string(++string_storage_counter_);
        result->data.set_pointer(std::move(pointer));
        return result;
    }

    [[nodiscard]] OptionalFunctionRef find_callable(const std::string& name, const std::vector<std::shared_ptr<Cell>>& args,
                                                bool require_constexpr) {
        auto it = functions_by_name_.find(name);
        if (it == functions_by_name_.end()) return {};
        for (std::size_t fn_index : it->second) {
            const Function& fn = program_.functions[fn_index];
            if (!fn.body) continue;
            if (require_constexpr && fn.eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn.params.size() != args.size()) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn.params[i].type, args[i], require_constexpr)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return make_function_ref(fn);
        }
        return {};
    }

    [[nodiscard]] OptionalFunctionRef find_single_argument_converting_constructor(const std::string& class_name,
                                                                              const std::shared_ptr<Cell>& arg,
                                                                              bool require_constexpr) {
        std::string constructor_name{};
        constructor_name += class_name;
        constructor_name += "_new";
        auto it = functions_by_name_.find(constructor_name);
        if (it == functions_by_name_.end()) return {};
        for (std::size_t fn_index : it->second) {
            const Function& fn = program_.functions[fn_index];
            if (!fn.body) continue;
            if (require_constexpr && fn.eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn.params.size() != 2) continue;
            const Type& param_type = fn.params[1].type;
            const Type& arg_type = arg->type;
            if (param_type.kind == TypeKind::Reference) {
                if (param_type.pointee && types_equal(*param_type.pointee, arg_type)) {
                    return make_function_ref(fn);
                }
            } else if (types_equal(param_type, arg_type)) {
                return make_function_ref(fn);
            }
        }
        return {};
    }

    [[nodiscard]] bool is_same_or_base_class_type(const Type& expected, const Type& actual) const {
        if (types_equal(expected, actual)) return true;
        if (expected.kind != TypeKind::Named || actual.kind != TypeKind::Named) return false;
        if (!is_class_name(expected.name) || !is_class_name(actual.name)) return false;
        std::string current = actual.name;
        while (true) {
            auto it = classes_by_name_.find(current);
            if (it == classes_by_name_.end()) return false;
            auto base = it->second->direct_ordinary_base();
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

    [[nodiscard]] bool constexpr_argument_matches_parameter(const Type& param_type, const std::shared_ptr<Cell>& arg,
                                                            bool require_constexpr) {
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
        if (is_same_or_base_class_type(param_type, arg_type)) return true;
        if (param_type.kind == TypeKind::Named && is_class_name(param_type.name)) {
            return find_single_argument_converting_constructor(param_type.name, arg, require_constexpr).has_value();
        }
        return false;
    }

    [[nodiscard]] OptionalFunctionRef find_constructor(const std::string& class_name,
                                                   const std::vector<std::shared_ptr<Cell>>& args,
                                                   bool require_constexpr) {
        std::string constructor_name{};
        constructor_name += class_name;
        constructor_name += "_new";
        auto it = functions_by_name_.find(constructor_name);
        if (it == functions_by_name_.end()) return {};
        for (std::size_t fn_index : it->second) {
            const Function& fn = program_.functions[fn_index];
            if (!fn.body) continue;
            if (require_constexpr && fn.eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn.params.size() != args.size() + 1) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const Type& param_type = fn.params[i + 1].type;
                const Type& arg_type = args[i]->type;
                if (param_type.kind == TypeKind::Reference) {
                    if (!param_type.pointee) {
                        params_match = false;
                        break;
                    }
                    if (types_equal(*param_type.pointee, arg_type)) continue;
                    if (param_type.pointee->is_const_qualified) {
                        Type unqualified = *param_type.pointee;
                        unqualified.is_const_qualified = false;
                        if (types_equal(unqualified, arg_type)) continue;
                    }
                    {
                        params_match = false;
                        break;
                    }
                } else if (!types_equal(param_type, arg_type)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return make_function_ref(fn);
        }
        return {};
    }

    [[nodiscard]] bool has_runtime_only_match(const std::string& name, const std::vector<std::shared_ptr<Cell>>& args) {
        auto it = functions_by_name_.find(name);
        if (it == functions_by_name_.end()) return false;
        for (std::size_t fn_index : it->second) {
            const Function& fn = program_.functions[fn_index];
            if (!fn.body || fn.eval_mode != FunctionEvalMode::RuntimeOnly || fn.params.size() != args.size()) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn.params[i].type, args[i], /*require_constexpr=*/false)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return true;
        }
        return false;
    }

    [[nodiscard]] bool is_constructor_function(const Function& fn) const {
        if (fn.member_owner_class.empty() || !fn.name.ends_with("_new") || fn.params.empty()) return false;
        const Type& this_param = fn.params[0].type;
        return this_param.kind == TypeKind::Reference && this_param.pointee != nullptr &&
               this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == fn.member_owner_class;
    }

    [[nodiscard]] std::expected<void, ConstexprError> apply_default_initializers_to_named_object(const std::shared_ptr<Cell>& object_cell, const Type& object_type,
                                                    const SourceLocation& loc) {
        if (object_type.kind != TypeKind::Named) return {};
        if (!object_cell->data.is_object()) return {};
        ObjectValue& object = object_cell->data.object;
        if (auto struct_it = structs_by_name_.find(object_type.name); struct_it != structs_by_name_.end()) {
            for (const StructField& field : struct_it->second->fields) {
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
        if (auto class_it = classes_by_name_.find(object_type.name); class_it != classes_by_name_.end()) {
            auto fields_result = collect_class_fields(*class_it->second);
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
        if (field_type.kind == TypeKind::Named &&
            (is_class_name(field_type.name) || structs_by_name_.contains(field_type.name)) && init.has_brace_args) {
            std::vector<std::shared_ptr<Cell>> arg_values{};
            arg_values.reserve(init.brace_args.size());
            for (const ExprPtr& arg : init.brace_args) {
                auto arg_result = evaluate_expr(*arg);
                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                arg_values.push_back(std::move(arg_result).value());
            }
            if (OptionalFunctionRef ctor_ref = find_constructor(field_type.name, arg_values, /*require_constexpr=*/true);
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
                            auto value_result = evaluate_expr(arg_expr);
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
                                auto value_result = evaluate_expr(arg_expr);
                                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                                bindings.push_back(Binding{std::move(value_result).value(), true});
                            }
                        }
                    } else {
                        auto value_result = evaluate_expr(arg_expr);
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
        }
        if (init.has_brace_args) {
            if (init.brace_args.empty()) return {};
            if (init.brace_args.size() != 1) {
                return std::unexpected(ConstexprError(loc, "brace-initialization of this member requires exactly one expression"));
            }
            auto value_result = evaluate_expr(*init.brace_args[0]);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            return copy_into(field_cell, std::move(value_result).value(), loc);
        }
        if (init.expr) {
            auto value_result = evaluate_expr(*init.expr);
            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
            return copy_into(field_cell, std::move(value_result).value(), loc);
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
        if (auto struct_it = structs_by_name_.find(fn.member_owner_class); struct_it != structs_by_name_.end()) {
            for (const StructField& field : struct_it->second->fields) {
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
        auto class_it = classes_by_name_.find(fn.member_owner_class);
        if (class_it == classes_by_name_.end()) {
            std::string message{};
            message += "missing constexpr class definition for '";
            message += fn.member_owner_class;
            message += "'";
            return std::unexpected(ConstexprError(fn.loc, message));
        }
        for (const ClassField& field : class_it->second->fields) {
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

    [[nodiscard]] bool has_user_defined_destructor(const std::string& class_name) const {
        std::string destructor_name{};
        destructor_name += class_name;
        destructor_name += "_delete";
        auto it = functions_by_name_.find(destructor_name);
        if (it == functions_by_name_.end()) return false;
        for (std::size_t fn_index : it->second) {
            if (program_.functions[fn_index].body) return true;
        }
        return false;
    }

    [[nodiscard]] std::expected<void, ConstexprError> reject_user_defined_destructor_execution(const Type& type, const SourceLocation& loc) const {
        if (type.kind != TypeKind::Named || !is_class_name(type.name) || !has_user_defined_destructor(type.name)) return {};
        std::string message{};
        message += "required constant evaluation cannot execute user-defined destructor of '";
        message += type.name;
        message += "'";
        return std::unexpected(ConstexprError(loc, message));
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> cast_value(const Type& target_type, const std::shared_ptr<Cell>& operand,
                                                   const SourceLocation& loc) {
        if (is_named_type(target_type, "double")) {
            auto result = as_double(operand, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return make_double_cell(result.value());
        }
        if (is_named_type(target_type, "bool")) {
            auto result = as_bool(operand, loc);
            if (!result.has_value()) return std::unexpected(std::move(result).error());
            return make_bool_cell(result.value());
        }
        if (is_named_type(target_type, "int") || is_named_type(target_type, "char")) {
            auto double_result = as_double(operand, loc);
            if (!double_result.has_value()) return std::unexpected(std::move(double_result).error());
            auto result = std::make_shared<Cell>();
            result->type = target_type;
            auto assign_result = checked_assign_integer(result, static_cast<std::int64_t>(double_result.value()), loc);
            if (!assign_result.has_value()) return std::unexpected(std::move(assign_result).error());
            return result;
        }
        return std::unexpected(ConstexprError(loc, "constexpr cast only supports builtin scalar targets in Phase D1"));
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_binary_numeric(const Expr& expr, const std::shared_ptr<Cell>& lhs,
                                                                const std::shared_ptr<Cell>& rhs) {
        if (is_floating_like(lhs->type) || is_floating_like(rhs->type)) {
            auto left_result = as_double(lhs, expr.loc);
            if (!left_result.has_value()) return std::unexpected(std::move(left_result).error());
            auto right_result = as_double(rhs, expr.loc);
            if (!right_result.has_value()) return std::unexpected(std::move(right_result).error());
            double left_double = left_result.value();
            double right_double = right_result.value();
            switch (expr.binary_op) {
                case BinaryOp::Add: return make_double_cell(left_double + right_double);
                case BinaryOp::Sub: return make_double_cell(left_double - right_double);
                case BinaryOp::Mul: return make_double_cell(left_double * right_double);
                case BinaryOp::Div: return make_double_cell(left_double / right_double);
                case BinaryOp::Eq: return make_bool_cell(left_double == right_double);
                case BinaryOp::Ne: return make_bool_cell(left_double != right_double);
                case BinaryOp::Lt: return make_bool_cell(left_double < right_double);
                case BinaryOp::Gt: return make_bool_cell(left_double > right_double);
                case BinaryOp::Le: return make_bool_cell(left_double <= right_double);
                case BinaryOp::Ge: return make_bool_cell(left_double >= right_double);
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
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly) {
            return std::unexpected(ConstexprError(loc, "immediate evaluation may only call constexpr/consteval functions"));
        }
        if (!fn.body) return std::unexpected(ConstexprError(loc, "cannot evaluate a declaration-only function at compile time"));
        ++call_depth_;
        if (call_depth_ > limits_.max_recursion_depth) {
            --call_depth_;
            return std::unexpected(ConstexprError(loc, "constexpr evaluation exceeded recursion budget"));
        }
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (fn.params[i].type.kind == TypeKind::Reference) continue;
            if (auto result = reject_user_defined_destructor_execution(fn.params[i].type, loc); !result.has_value()) {
                --call_depth_;
                return std::unexpected(std::move(result).error());
            }
        }
        frames_.emplace_back();
        std::unordered_map<std::string, Binding>& frame = frames_.back();
        for (std::size_t i = 0; i < fn.params.size(); ++i) frame.emplace(fn.params[i].name, std::move(bindings[i]));
        auto init_result = execute_constructor_member_initializers(fn);
        std::expected<ExecOutcome, ConstexprError> body_result{};
        if (init_result.has_value()) {
            body_result = execute_stmt(*fn.body, fn.return_type);
        }
        frames_.pop_back();
        --call_depth_;
        if (!init_result.has_value()) return std::unexpected(std::move(init_result).error());
        if (!body_result.has_value()) return std::unexpected(std::move(body_result).error());
        if (body_result.value().flow == ExecFlow::Return) {
            if (body_result.value().return_value) return clone_cell(body_result.value().return_value);
            return make_default_cell(fn.return_type, loc);
        }
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
            const Param& param = fn.params[i];
            const Expr& arg_expr = *args[i];
            if (param.type.kind == TypeKind::Reference) {
                if (param.type.is_rvalue_ref) {
                    auto value_result = evaluate_expr(arg_expr);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    std::shared_ptr<Cell> value = std::move(value_result).value();
                    if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, value->type) &&
                        !types_equal(*param.type.pointee, value->type)) {
                        auto cloned = clone_cell_as_type(value, *param.type.pointee, loc);
                        if (!cloned.has_value()) return std::unexpected(std::move(cloned).error());
                        value = std::move(cloned).value();
                    }
                    bindings.push_back(Binding{value, false});
                    continue;
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
                        auto value_result = evaluate_expr(arg_expr);
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
                auto value_result = evaluate_expr(arg_expr);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                std::shared_ptr<Cell> value = std::move(value_result).value();
                if (is_same_or_base_class_type(param.type, value->type) && !types_equal(param.type, value->type)) {
                    auto cloned = clone_cell_as_type(value, param.type, loc);
                    if (!cloned.has_value()) return std::unexpected(std::move(cloned).error());
                    bindings.push_back(Binding{std::move(cloned).value(), false});
                } else if (!types_equal(param.type, value->type) &&
                           param.type.kind == TypeKind::Named && is_class_name(param.type.name)) {
                    OptionalFunctionRef ctor_ref =
                        find_single_argument_converting_constructor(param.type.name, value, /*require_constexpr=*/true);
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
                                                       bool require_constexpr) {
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
        if (receiver_value->type.kind != TypeKind::Named || !is_class_name(receiver_value->type.name)) return {};

        std::string full_name{};
        full_name += receiver_value->type.name;
        full_name += "_";
        full_name += method_name;
        auto it = functions_by_name_.find(full_name);
        if (it == functions_by_name_.end()) return {};
        for (std::size_t fn_index : it->second) {
            const Function& fn = program_.functions[fn_index];
            if (!fn.body) continue;
            if (require_constexpr && fn.eval_mode == FunctionEvalMode::RuntimeOnly) continue;
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
                if (!constexpr_argument_matches_parameter(fn.params[i + 1].type, arg_values[i], require_constexpr)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return make_function_ref(fn);
        }
        return {};
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_constructor_expr(const Expr& expr) {
        Type object_type = named_type(expr.name);
        auto object_result = make_default_cell(object_type, expr.loc);
        if (!object_result.has_value()) return std::unexpected(std::move(object_result).error());
        auto object = std::move(object_result).value();
        std::vector<std::shared_ptr<Cell>> arg_values{};
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) {
            auto arg_result = evaluate_expr(*arg);
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            arg_values.push_back(std::move(arg_result).value());
        }
        OptionalFunctionRef ctor_ref = find_constructor(expr.name, arg_values, /*require_constexpr=*/true);
        if (!ctor_ref.has_value()) {
            if (expr.args.empty() &&
                (classes_by_name_.contains(expr.name) || structs_by_name_.contains(expr.name))) {
                if (auto result = apply_default_initializers_to_named_object(object, object_type, expr.loc); !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                return object;
            }
            std::string constructor_name{};
            constructor_name += expr.name;
            constructor_name += "_new";
            if (has_runtime_only_match(constructor_name, arg_values)) {
                return std::unexpected(ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval constructors"));
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
                    auto value_result = evaluate_expr(arg_expr);
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
                        auto value_result = evaluate_expr(arg_expr);
                        if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                        bindings.push_back(Binding{std::move(value_result).value(), true});
                    }
                }
            } else {
                auto value_result = evaluate_expr(arg_expr);
                if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                bindings.push_back(Binding{std::move(value_result).value(), false});
            }
        }
        auto call_result = call_function(ctor, std::move(bindings), expr.loc);
        if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
        return object;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_call_expr(const Expr& expr) {
        if (expr.lhs) {
            std::vector<std::shared_ptr<Cell>> arg_values{};
            arg_values.reserve(expr.args.size());
            for (const ExprPtr& arg : expr.args) {
                auto arg_result = evaluate_expr(*arg);
                if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
                arg_values.push_back(std::move(arg_result).value());
            }
            auto fn_result = find_method_callable(*expr.lhs, expr.name, arg_values, /*require_constexpr=*/true);
            if (!fn_result.has_value()) return std::unexpected(std::move(fn_result).error());
            OptionalFunctionRef method_ref = fn_result.value();
            if (!method_ref.has_value()) {
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
        }
        if (is_class_name(expr.name)) return evaluate_constructor_expr(expr);
        std::vector<std::shared_ptr<Cell>> arg_values{};
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) {
            auto arg_result = evaluate_expr(*arg);
            if (!arg_result.has_value()) return std::unexpected(std::move(arg_result).error());
            arg_values.push_back(std::move(arg_result).value());
        }
        OptionalFunctionRef callee_ref = find_callable(expr.name, arg_values, /*require_constexpr=*/true);
        if (!callee_ref.has_value()) {
            if (has_runtime_only_match(expr.name, arg_values)) {
                return std::unexpected(ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval functions"));
            }
            std::string message{};
            message += "no constexpr/consteval overload of '";
            message += expr.name;
            message += "' matches this immediate call";
            return std::unexpected(ConstexprError(expr.loc, message));
        }
        return call_with_expr_args(callee_ref->get(), expr.args, expr.loc);
    }

    [[nodiscard]] std::optional<std::reference_wrapper<const EnumDef>> find_enum_for_variant(const std::string& variant_name) const {
        for (const EnumDef& def : program_.enums) {
            for (const EnumVariant& variant : def.variants) {
                if (variant.name == variant_name) {
                    return std::optional<std::reference_wrapper<const EnumDef>>{std::reference_wrapper<const EnumDef>{def}};
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::optional<Type> infer_unevaluated_expr_type(const Expr& expr) {
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return named_type("int");
            case ExprKind::FloatLiteral: return named_type("double");
            case ExprKind::BoolLiteral: return named_type("bool");
            case ExprKind::CharLiteral: return named_type("char");
            case ExprKind::TypeTrait: return named_type("bool");
            case ExprKind::Alignof:
            case ExprKind::Sizeof:
                return named_type("size_t");
            case ExprKind::ValueInit:
                return expr.type;
            case ExprKind::Destroy: return named_type("void");
            case ExprKind::StringLiteral: return make_const_char_pointer_type();
            case ExprKind::Identifier: {
                // Speculative: prefer a real binding; an identifier that
                // isn't currently bound (e.g. an enum variant name used
                // as a value) falls back to enum-variant lookup, else
                // nullopt -- exactly like the original try/catch(const
                // ConstexprError&) fallback.
                auto binding_result = lookup_binding(expr.name, expr.loc);
                if (binding_result.has_value()) return binding_result.value().cell->type;
                if (std::optional<std::reference_wrapper<const EnumDef>> enum_def = find_enum_for_variant(expr.name);
                    enum_def.has_value()) {
                    return named_type(enum_def->get().name);
                }
                return std::nullopt;
            }
            case ExprKind::Move:
                return expr.lhs ? infer_unevaluated_expr_type(*expr.lhs) : std::nullopt;
            case ExprKind::New: {
                Type result{};
                result.kind = TypeKind::Pointer;
                result.pointee = std::make_shared<Type>(expr.type);
                result.is_mutable_pointee = true;
                return result;
            }
            case ExprKind::Delete:
                return named_type("void");
            case ExprKind::Cast:
                return expr.type;
            case ExprKind::Lambda:
                return expr.name.empty() ? std::nullopt : std::optional<Type>(named_type(expr.name));
            case ExprKind::Conditional: {
                if (!expr.rhs || !expr.third) return std::nullopt;
                std::optional<Type> lhs_type = infer_unevaluated_expr_type(*expr.rhs);
                std::optional<Type> rhs_type = infer_unevaluated_expr_type(*expr.third);
                if (!lhs_type.has_value() || !rhs_type.has_value() || !types_equal(*lhs_type, *rhs_type)) return std::nullopt;
                return lhs_type;
            }
            case ExprKind::Member: {
                std::optional<Type> base = infer_unevaluated_expr_type(*expr.lhs);
                if (!base.has_value()) return std::nullopt;
                bool base_is_sized_span = base->kind == TypeKind::Span && base->pointee != nullptr;
                if (base_is_sized_span && expr.name == "size") {
                    return named_type("size_t");
                }
                const Type& base_named = base->kind == TypeKind::Reference ? *base->pointee : *base;
                if (base_named.kind != TypeKind::Named) return std::nullopt;
                if (auto struct_it = structs_by_name_.find(base_named.name); struct_it != structs_by_name_.end()) {
                    for (const StructField& field : struct_it->second->fields) {
                        if (field.name == expr.name) return field.type;
                    }
                }
                if (auto class_it = classes_by_name_.find(base_named.name); class_it != classes_by_name_.end()) {
                    // Best-effort: a missing base-class definition is a
                    // real diagnostic elsewhere (e.g. make_default_cell);
                    // here it just means this type can't be inferred.
                    auto fields_result = collect_class_fields(*class_it->second);
                    if (fields_result.has_value()) {
                        for (const ClassField& field : fields_result.value()) {
                            if (field.name == expr.name) return field.type.kind == TypeKind::Reference ? *field.type.pointee : field.type;
                        }
                    }
                }
                return std::nullopt;
            }
            case ExprKind::Subscript: {
                std::optional<Type> base = infer_unevaluated_expr_type(*expr.lhs);
                if (!base.has_value()) return std::nullopt;
                const Type& effective = base->kind == TypeKind::Reference && base->pointee ? *base->pointee : *base;
                if (effective.kind == TypeKind::Array && effective.element) return *effective.element;
                if ((effective.kind == TypeKind::Pointer || effective.kind == TypeKind::Span) && effective.pointee) {
                    return *effective.pointee;
                }
                return std::nullopt;
            }
            case ExprKind::Unary:
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
            case ExprKind::Binary:
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
            case ExprKind::Call:
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
                    auto it = functions_by_name_.find(full_name);
                    if (it == functions_by_name_.end()) return std::nullopt;
                    for (std::size_t fn_index : it->second) {
                        const Function& fn = program_.functions[fn_index];
                        if (fn.params.size() == expr.args.size() + 1) return fn.return_type;
                    }
                    return std::nullopt;
                }
                if (is_class_name(expr.name)) return named_type(expr.name);
                if (auto it = functions_by_name_.find(expr.name); it != functions_by_name_.end()) {
                    for (std::size_t fn_index : it->second) {
                        const Function& fn = program_.functions[fn_index];
                        if (fn.params.size() == expr.args.size()) return fn.return_type;
                    }
                }
                return std::nullopt;
            case ExprKind::PackExpansion:
            case ExprKind::Fold:
                return std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::expected<std::shared_ptr<Cell>, ConstexprError> evaluate_expr(const Expr& expr) {
        if (auto result = tick(expr.loc, "evaluating an expression"); !result.has_value()) return std::unexpected(std::move(result).error());
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return make_scalar_cell(named_type("int"), expr.int_value);
            case ExprKind::FloatLiteral: return make_double_cell(expr.float_value);
            case ExprKind::BoolLiteral: return make_bool_cell(expr.bool_value);
            case ExprKind::CharLiteral: return make_scalar_cell(named_type("char"), expr.int_value);
            case ExprKind::StringLiteral: return make_string_literal_pointer(expr);
            case ExprKind::Destroy:
                return std::unexpected(ConstexprError(expr.loc, "explicit destructor calls are not supported during constant evaluation"));
            case ExprKind::ValueInit:
                return make_default_cell(expr.type, expr.loc);
            case ExprKind::Alignof: {
                if (auto result = reject_if_incomplete(expr.type, expr.loc, "alignof"); !result.has_value()) {
                    return std::unexpected(std::move(result).error());
                }
                std::optional<TypeLayoutInfo> layout = layout_of_type(program_, expr.type);
                if (!layout.has_value()) return std::unexpected(ConstexprError(expr.loc, "cannot apply 'alignof' to this type in this version"));
                return make_scalar_cell(named_type("size_t"), static_cast<std::int64_t>(layout->abi_align_bytes));
            }
            case ExprKind::Sizeof: {
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
            }
            case ExprKind::Identifier: {
                auto binding_result = lookup_binding(expr.name, expr.loc);
                if (!binding_result.has_value()) return std::unexpected(std::move(binding_result).error());
                return clone_cell(binding_result.value().cell);
            }
            case ExprKind::Conditional: {
                auto cond_result = evaluate_expr(*expr.lhs);
                if (!cond_result.has_value()) return std::unexpected(std::move(cond_result).error());
                auto cond_bool = as_bool(cond_result.value(), expr.loc);
                if (!cond_bool.has_value()) return std::unexpected(std::move(cond_bool).error());
                return cond_bool.value() ? evaluate_expr(*expr.rhs) : evaluate_expr(*expr.third);
            }
            case ExprKind::Member: {
                auto base_result = evaluate_expr(*expr.lhs);
                if (!base_result.has_value()) return std::unexpected(std::move(base_result).error());
                std::shared_ptr<Cell> base = std::move(base_result).value();
                if (base->data.is_span() && expr.name == "size") {
                    return make_checked_int_cell(base->data.span.size, expr.loc);
                }
                auto lvalue_result = resolve_lvalue(expr);
                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                return clone_cell(lvalue_result.value().cell);
            }
            case ExprKind::Subscript: {
                auto lvalue_result = resolve_lvalue(expr);
                if (!lvalue_result.has_value()) return std::unexpected(std::move(lvalue_result).error());
                return clone_cell(lvalue_result.value().cell);
            }
            case ExprKind::Call:
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
            case ExprKind::Cast: {
                auto operand_result = evaluate_expr(*expr.lhs);
                if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                return cast_value(expr.type, operand_result.value(), expr.loc);
            }
            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Assign || expr.binary_op == BinaryOp::AddAssign ||
                    expr.binary_op == BinaryOp::SubAssign || expr.binary_op == BinaryOp::MulAssign ||
                    expr.binary_op == BinaryOp::DivAssign) {
                    auto target_result = resolve_lvalue(*expr.lhs);
                    if (!target_result.has_value()) return std::unexpected(std::move(target_result).error());
                    LValue target = std::move(target_result).value();
                    if (target.read_only) return std::unexpected(ConstexprError(expr.loc, "cannot assign through a const/constexpr binding"));
                    auto value_result = evaluate_expr(*expr.rhs);
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
                if (expr.binary_op == BinaryOp::And) {
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
                }
                if (expr.binary_op == BinaryOp::Or) {
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
                }
                {
                    auto lhs_result = evaluate_expr(*expr.lhs);
                    if (!lhs_result.has_value()) return std::unexpected(std::move(lhs_result).error());
                    auto rhs_result = evaluate_expr(*expr.rhs);
                    if (!rhs_result.has_value()) return std::unexpected(std::move(rhs_result).error());
                    return evaluate_binary_numeric(expr, lhs_result.value(), rhs_result.value());
                }
            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Neg: {
                        auto operand_result = evaluate_expr(*expr.lhs);
                        if (!operand_result.has_value()) return std::unexpected(std::move(operand_result).error());
                        std::shared_ptr<Cell> operand = std::move(operand_result).value();
                        if (is_floating_like(operand->type)) {
                            auto double_result = as_double(operand, expr.loc);
                            if (!double_result.has_value()) return std::unexpected(std::move(double_result).error());
                            return make_double_cell(-double_result.value());
                        }
                        auto integer_result = as_integer(operand, expr.loc);
                        if (!integer_result.has_value()) return std::unexpected(std::move(integer_result).error());
                        std::int64_t negated_value = integer_result.value();
                        if (negated_value == int64_min_value) {
                            return std::unexpected(ConstexprError(expr.loc, "constexpr integer overflow"));
                        }
                        return make_checked_int_cell(-negated_value, expr.loc);
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
                break;
            case ExprKind::TypeTrait:
                return std::unexpected(ConstexprError(expr.loc, "constexpr type traits are deferred to a later phase"));
            case ExprKind::New:
            case ExprKind::Delete:
            case ExprKind::Move:
            case ExprKind::PackExpansion:
            case ExprKind::Lambda:
            case ExprKind::Fold:
                break;
        }
        return std::unexpected(ConstexprError(expr.loc, "expression kind is not yet supported by the constexpr evaluator in Phase D1"));
    }

    [[nodiscard]] std::expected<ExecOutcome, ConstexprError> execute_stmt(const Stmt& stmt, const Type& return_type) {
        if (auto result = tick(stmt.loc, "executing a statement"); !result.has_value()) return std::unexpected(std::move(result).error());
        switch (stmt.kind) {
            case StmtKind::VarDecl: {
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
                    OptionalFunctionRef ctor_ref = find_callable(constructor_name, arg_values, /*require_constexpr=*/true);
                    if (!ctor_ref.has_value()) {
                        if (stmt.ctor_args.empty() &&
                            (classes_by_name_.contains(stmt.type.name) || structs_by_name_.contains(stmt.type.name))) {
                            if (auto result = apply_default_initializers_to_named_object(cell, stmt.type, stmt.loc);
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
                            auto value_result = evaluate_expr(arg_expr);
                            if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                            ctor_bindings.push_back(
                                Binding{std::move(value_result).value(),
                                        param.type.kind == TypeKind::Reference && !param.type.is_mutable_ref});
                        }
                    }
                    auto call_result = call_function(ctor, std::move(ctor_bindings), stmt.loc);
                    if (!call_result.has_value()) return std::unexpected(std::move(call_result).error());
                } else if (stmt.init) {
                    auto value_result = evaluate_expr(*stmt.init);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    if (auto result = copy_into(cell, std::move(value_result).value(), stmt.loc); !result.has_value()) {
                        return std::unexpected(std::move(result).error());
                    }
                }
                frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                return ExecOutcome{};
            }
            case StmtKind::Return: {
                if (stmt.expr) {
                    auto value_result = evaluate_expr(*stmt.expr);
                    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
                    return ExecOutcome{ExecFlow::Return, std::move(value_result).value()};
                }
                if (is_named_type(return_type, "void")) {
                    return ExecOutcome{ExecFlow::Return, nullptr};
                }
                auto default_result = make_default_cell(return_type, stmt.loc);
                if (!default_result.has_value()) return std::unexpected(std::move(default_result).error());
                return ExecOutcome{ExecFlow::Return, std::move(default_result).value()};
            }
            case StmtKind::ExprStmt:
                if (stmt.expr) {
                    auto expr_stmt_result = evaluate_expr(*stmt.expr);
                    if (!expr_stmt_result.has_value()) return std::unexpected(std::move(expr_stmt_result).error());
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
            case StmtKind::While: {
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
            }
            case StmtKind::Switch: {
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
            }
            case StmtKind::Break: return ExecOutcome{ExecFlow::Break, nullptr};
            case StmtKind::Continue: return ExecOutcome{ExecFlow::Continue, nullptr};
            case StmtKind::Fallthrough: return ExecOutcome{};
            case StmtKind::Block: {
                if (stmt.is_unsafe) return std::unexpected(ConstexprError(stmt.loc, "unsafe blocks are not allowed in constant evaluation"));
                frames_.emplace_back();
                std::expected<ExecOutcome, ConstexprError> result = ExecOutcome{};
                for (const StmtPtr& nested : stmt.statements) {
                    result = execute_stmt(*nested, return_type);
                    if (!result.has_value() || result.value().flow != ExecFlow::Normal) break;
                }
                frames_.pop_back();
                return result;
            }
        }
        return ExecOutcome{};
    }
};

[[nodiscard]] bool has_unique_consteval_function(const Program& program, const Expr& expr) {
    if (expr.kind != ExprKind::Call || expr.lhs) return false;
    bool found_one = false;
    for (const Function& fn : program.functions) {
        if (fn.name != expr.name || fn.eval_mode != FunctionEvalMode::Consteval) continue;
        if (fn.params.size() != expr.args.size()) continue;
        if (found_one) return false;
        found_one = true;
    }
    return found_one;
}

[[nodiscard]] bool expr_depends_on_runtime_bindings(const Expr& expr) {
    switch (expr.kind) {
        case ExprKind::Identifier:
            return true;
        case ExprKind::IntegerLiteral:
        case ExprKind::FloatLiteral:
        case ExprKind::BoolLiteral:
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

[[nodiscard]] std::expected<void, ConstexprError> rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value) {
    if (is_named_type(value->type, "int")) {
        expr.kind = ExprKind::IntegerLiteral;
        expr.int_value = value->data.int_value;
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "char")) {
        expr.kind = ExprKind::CharLiteral;
        expr.int_value = value->data.int_value;
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "bool")) {
        expr.kind = ExprKind::BoolLiteral;
        expr.bool_value = value->data.bool_value;
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.name.clear();
    } else if (is_named_type(value->type, "double")) {
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
            return std::unexpected(ConstexprError(expr.loc, "Phase D1 cannot yet lower this constexpr pointer result back into source form"));
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
        return std::unexpected(ConstexprError(expr.loc, "Phase D1 can only lower scalar and string-literal immediate results"));
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
    if (has_unique_consteval_function(program, expr) && !expr_depends_on_runtime_bindings(expr)) {
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
            if (auto result = resolve_array_bounds_type_dependencies(global.decl->type); !result.has_value()) return result;
        }
        for (Function& fn : program_.functions) {
            if (fn.is_generic_template) continue;
            if (!fn.member_owner_class.empty() && generic_template_owner_names_.contains(fn.member_owner_class)) {
                continue;
            }
            for (Param& param : fn.params) {
                if (auto result = resolve_array_bounds_type_dependencies(param.type); !result.has_value()) return result;
            }
            if (auto result = resolve_array_bounds_type_dependencies(fn.return_type); !result.has_value()) return result;
            if (fn.body) {
                // ch05 §9.4 (local-constexpr-as-array-bound gap fix):
                // opens this function's own local constant-evaluation
                // frame (see ConstexprEngine::begin_local_array_bound_scope)
                // so a local `constexpr`/const-with-init declaration seen
                // while walking this body below becomes visible to a
                // later array bound in the same function, exactly like it
                // already is for `alignas` via validate_constexpr_locals.
                // Reset fresh per function -- never leaks across functions.
                engine_.begin_local_array_bound_scope();
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
            if (auto result = resolve_type_dependencies(global.decl->type); !result.has_value()) return result;
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
                engine_.resolve_root_alignment_specs(global.decl->alignment_specs, layout->abi_align_bytes, global.decl->loc,
                                                     alignment_context);
            if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
            global.decl->resolved_alignment = alignment_result.value();
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
    [[nodiscard]] std::expected<void, ConstexprError> resolve_array_bounds_type_dependencies(Type& type) {
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
                if (type.pointee) return resolve_array_bounds_type_dependencies(*type.pointee);
                return {};
            case TypeKind::Array:
                if (type.element) {
                    if (auto result = resolve_array_bounds_type_dependencies(*type.element); !result.has_value()) return result;
                }
                if (type.array_size_expr) {
                    auto bound_result = engine_.resolve_root_array_bound(*type.array_size_expr);
                    if (!bound_result.has_value()) return std::unexpected(std::move(bound_result).error());
                    type.array_size = bound_result.value();
                    type.array_size_expr.reset();
                }
                return {};
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) {
                    if (auto result = resolve_array_bounds_type_dependencies(*type.function_return); !result.has_value()) {
                        return result;
                    }
                }
                for (Type& param : type.function_params) {
                    if (auto result = resolve_array_bounds_type_dependencies(param); !result.has_value()) return result;
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
            if (auto result = resolve_array_bounds_type_dependencies(field.type); !result.has_value()) return result;
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
            if (auto result = resolve_array_bounds_type_dependencies(base_type); !result.has_value()) return result;
        }
        for (ClassField& field : def.fields) {
            if (auto result = resolve_array_bounds_type_dependencies(field.type); !result.has_value()) return result;
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

    [[nodiscard]] std::expected<void, ConstexprError> resolve_type_dependencies(Type& type) {
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
                if (type.pointee) return resolve_type_dependencies(*type.pointee);
                return {};
            case TypeKind::Array:
                if (type.element) {
                    if (auto result = resolve_type_dependencies(*type.element); !result.has_value()) return result;
                }
                // ch05 §9.4: this array's own bound (not its element's --
                // that was just handled by the recursive call above)
                // must be resolved before any layout_of_type call below
                // (natural_field_alignment/natural_struct_alignment/
                // natural_class_alignment, and this same function's own
                // callers) ever reads `array_size`.
                if (type.array_size_expr) {
                    auto bound_result = engine_.resolve_root_array_bound(*type.array_size_expr);
                    if (!bound_result.has_value()) return std::unexpected(std::move(bound_result).error());
                    type.array_size = bound_result.value();
                    type.array_size_expr.reset();
                }
                return {};
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) {
                    if (auto result = resolve_type_dependencies(*type.function_return); !result.has_value()) return result;
                }
                for (Type& param : type.function_params) {
                    if (auto result = resolve_type_dependencies(param); !result.has_value()) return result;
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
            if (auto result = resolve_type_dependencies(field.type); !result.has_value()) return result;
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
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, field.loc,
                                                     alignment_context);
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
            engine_.resolve_root_alignment_specs(def.alignment_specs, natural_align, def.loc,
                                                 def_alignment_context);
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
            if (auto result = resolve_type_dependencies(base_type); !result.has_value()) return result;
        }
        for (ClassField& field : def.fields) {
            if (auto result = resolve_type_dependencies(field.type); !result.has_value()) return result;
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
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, field.loc,
                                                     alignment_context);
            if (!alignment_result.has_value()) return std::unexpected(std::move(alignment_result).error());
            field.resolved_alignment = alignment_result.value();
        }
        std::uint64_t natural_align = natural_class_alignment(def);
        std::string def_alignment_context{};
        def_alignment_context += "class '";
        def_alignment_context += def.name;
        def_alignment_context += "'";
        auto def_alignment_result =
            engine_.resolve_root_alignment_specs(def.alignment_specs, natural_align, def.loc, def_alignment_context);
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
    for (Function& fn : program.functions) {
        if (fn.eval_mode == FunctionEvalMode::Consteval && !fn.name.ends_with("_new")) fn.body.reset();
    }
    return {};
}

[[nodiscard]] std::expected<ConstexprValue, ConstexprError> evaluate_immediate_expr(const Program& program, const Expr& expr, ConstexprLimits limits) {
    ConstexprEngine engine{program, limits};
    auto value_result = engine.evaluate_root_expr(expr);
    if (!value_result.has_value()) return std::unexpected(std::move(value_result).error());
    return snapshot_constexpr_value(value_result.value(), expr.loc);
}

} // namespace scpp
