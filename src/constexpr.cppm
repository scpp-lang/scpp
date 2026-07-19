module;

export module scpp.constexpr_engine;

import std;
import scpp.ast;

export namespace scpp {

struct ConstexprLimits {
    int max_steps = 1000000;
    int max_recursion_depth = 512;
    int max_loop_iterations = 262144;
};

struct ConstexprError : std::runtime_error {
    SourceLocation loc;

    ConstexprError(const SourceLocation& loc, const std::string& message)
        : std::runtime_error(std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " + message),
          loc(loc) {}
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

struct ConstexprValue {
    Type type;
    ConstexprValueKind kind = ConstexprValueKind::Void;
    long long int_value = 0;
    double double_value = 0.0;
    bool bool_value = false;
    std::string string_value;
    std::vector<std::pair<std::string, std::shared_ptr<ConstexprValue>>> object_fields;
    std::vector<ConstexprValue> elements;
};

void fold_immediate_calls(Program& program, ConstexprLimits limits = {});
[[nodiscard]] ConstexprValue evaluate_immediate_expr(const Program& program, const Expr& expr,
                                                     ConstexprLimits limits = {});

} // namespace scpp

namespace scpp {
namespace {

struct Cell;

void rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value);
[[nodiscard]] ConstexprValue snapshot_constexpr_value(const std::shared_ptr<Cell>& value, const SourceLocation& loc);

struct PointerValue {
    std::shared_ptr<Cell> storage;
    std::string storage_id;
    long long index = 0;
};

struct SpanValue {
    PointerValue pointer;
    long long size = 0;
};

struct ObjectValue {
    std::string type_name;
    std::unordered_map<std::string, std::shared_ptr<Cell>> fields;
};

struct ArrayValue {
    Type element_type;
    std::vector<std::shared_ptr<Cell>> elements;
};

using CellData = std::variant<std::monostate, long long, double, bool, PointerValue, SpanValue, ObjectValue, ArrayValue>;

struct Cell {
    Type type;
    CellData data;
};

struct Binding {
    std::shared_ptr<Cell> cell;
    bool read_only = false;
};

struct LValue {
    std::shared_ptr<Cell> cell;
    bool read_only = false;
};

struct ExprRewrite {
    Expr* target = nullptr;
    std::shared_ptr<Cell> value;
};

struct ReturnSignal {
    std::shared_ptr<Cell> value;
};

struct BreakSignal {};
struct ContinueSignal {};

[[nodiscard]] bool types_equal(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
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
    Type result;
    result.kind = TypeKind::Pointer;
    result.pointee = std::make_shared<Type>(named_type("char"));
    result.is_mutable_pointee = false;
    return result;
}

[[nodiscard]] bool is_named_type(const Type& type, std::string_view name) {
    return type.kind == TypeKind::Named && type.name == name && type.template_args.empty();
}

[[nodiscard]] bool is_integral_named_type(std::string_view name) {
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
    Type type;
    type.kind = TypeKind::Pointer;
    type.pointee = std::make_shared<Type>(pointee);
    type.is_mutable_pointee = is_mutable_pointee;
    return type;
}

[[nodiscard]] std::shared_ptr<Cell> dereference_pointer(const PointerValue& pointer, const Type& pointer_type,
                                                        const SourceLocation& loc) {
    if (!pointer.storage) throw ConstexprError(loc, "constexpr dereference requires a non-null pointer");
    if (pointer_type.kind != TypeKind::Pointer || !pointer_type.pointee) {
        throw ConstexprError(loc, "malformed constexpr pointer type");
    }
    if (types_equal(*pointer_type.pointee, pointer.storage->type)) {
        if (pointer.index != 0) {
            throw ConstexprError(loc, "constexpr pointer arithmetic escaped the pointed-to object");
        }
        return pointer.storage;
    }
    auto* array = std::get_if<ArrayValue>(&pointer.storage->data);
    if (!array) throw ConstexprError(loc, "constexpr pointer does not point to supported storage");
    if (!types_equal(*pointer_type.pointee, array->element_type)) {
        throw ConstexprError(loc, "constexpr pointer element type does not match the pointed-to storage");
    }
    if (pointer.index < 0 || static_cast<std::size_t>(pointer.index) >= array->elements.size()) {
        throw ConstexprError(loc, "constexpr dereference out of bounds");
    }
    return array->elements[static_cast<std::size_t>(pointer.index)];
}

class ConstexprEngine {
public:
    ConstexprEngine(const Program& program, ConstexprLimits limits)
        : program_(program), limits_(limits) {
        for (std::size_t i = 0; i < program_.functions.size(); ++i) functions_by_name_[program_.functions[i].name].push_back(i);
        for (const ClassDef& def : program_.classes) classes_by_name_.emplace(def.name, &def);
        for (const StructDef& def : program_.structs) structs_by_name_.emplace(def.name, &def);
        for (const GlobalVar& global : program_.globals) {
            if (global.decl != nullptr) globals_by_name_.emplace(global.decl->var_name, &global);
        }
    }

    std::shared_ptr<Cell> evaluate_root_expr(const Expr& expr) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return evaluate_expr(expr);
    }

    void validate_constexpr_locals(Function& fn) {
        if (!fn.body) return;
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        frames_.push_back({});
        validate_constexpr_stmt_tree(*fn.body);
        frames_.pop_back();
    }

    std::uint64_t resolve_root_alignment_specs(const std::vector<AlignmentSpecifier>& specs, std::uint64_t natural_alignment,
                                               const SourceLocation& loc, std::string_view what) {
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
    [[nodiscard]] long long evaluate_and_validate_array_bound(const Expr& expr) {
        std::shared_ptr<Cell> value = evaluate_expr(expr);
        if (!is_integer_like(value->type)) {
            throw ConstexprError(expr.loc,
                                 "array bound must be a converted constant expression of type 'std::size_t'");
        }
        long long raw = as_integer(value, expr.loc);
        if (raw <= 0) {
            throw ConstexprError(expr.loc, "array bound must be greater than zero (got " + std::to_string(raw) + ")");
        }
        return raw;
    }

    // Top-level entry point: resets evaluation state (mirroring
    // `resolve_root_alignment_specs`) before evaluating. Only safe to call
    // when no other evaluation is already in progress on this engine.
    [[nodiscard]] long long resolve_root_array_bound(const Expr& expr) {
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
    void resolve_array_bounds_in_type(Type& type) {
        switch (type.kind) {
            case TypeKind::Array:
                if (type.element) resolve_array_bounds_in_type(*type.element);
                if (type.array_size_expr) {
                    long long resolved = evaluate_and_validate_array_bound(*type.array_size_expr);
                    type.array_size = resolved;
                    type.array_size_expr.reset();
                }
                return;
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) resolve_array_bounds_in_type(*type.pointee);
                return;
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) resolve_array_bounds_in_type(*type.function_return);
                for (Type& param : type.function_params) resolve_array_bounds_in_type(param);
                return;
            case TypeKind::Named:
                return;
        }
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
        frames_.push_back({});
    }

    void end_local_array_bound_scope() { frames_.pop_back(); }

    void push_local_array_bound_scope() { frames_.push_back({}); }
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
    void bind_local_constant_for_array_bounds(const Stmt& stmt) {
        if (stmt.is_constexpr) {
            execute_stmt(stmt, named_type("void"));
        } else if (stmt.is_const && (stmt.init || stmt.has_ctor_args)) {
            try {
                execute_stmt(stmt, named_type("void"));
            } catch (const ConstexprError&) {
            }
        }
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
    ConstexprLimits limits_;
    int steps_ = 0;
    int call_depth_ = 0;
    int string_storage_counter_ = 0;
    std::vector<std::unordered_map<std::string, Binding>> frames_;
    std::unordered_map<std::string, std::vector<std::size_t>> functions_by_name_;
    std::unordered_map<std::string, const ClassDef*> classes_by_name_;
    std::unordered_map<std::string, const StructDef*> structs_by_name_;
    std::unordered_set<std::string> incomplete_type_names_;
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
    std::unordered_map<std::string, const GlobalVar*> globals_by_name_;
    std::unordered_map<std::string, std::shared_ptr<Cell>> resolved_global_constants_;
    std::unordered_set<std::string> globals_resolving_;

    // ch05 §9.4(6): `struct Self { char buf[sizeof(Self)]; };` -- Self is
    // incomplete at this point, so evaluating its size/alignment must be
    // rejected rather than silently computed from a partially-resolved
    // definition (see mark_type_incomplete/mark_type_complete above).
    void reject_if_incomplete(const Type& queried_type, const SourceLocation& loc, std::string_view op) const {
        if (queried_type.kind == TypeKind::Named && incomplete_type_names_.contains(queried_type.name)) {
            throw ConstexprError(loc, "cannot apply '" + std::string(op) + "' to '" + queried_type.name +
                                          "': it is still an incomplete type at this point");
        }
    }

    void tick(const SourceLocation& loc, std::string_view what) {
        ++steps_;
        if (steps_ > limits_.max_steps) {
            throw ConstexprError(loc, "constexpr evaluation exceeded step budget while " + std::string(what));
        }
    }

    [[nodiscard]] static bool is_power_of_two(std::uint64_t value) {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]] std::uint64_t evaluate_alignment_operand(const AlignmentSpecifier& spec) {
        if (spec.operand_is_type) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, spec.type);
            if (!layout.has_value()) {
                throw ConstexprError(spec.loc, "cannot apply 'alignas' to this type in this version");
            }
            return layout->abi_align_bytes;
        }
        if (!spec.expr) {
            throw ConstexprError(spec.loc, "internal error: malformed alignas operand");
        }
        std::shared_ptr<Cell> value = evaluate_expr(*spec.expr);
        if (!is_integer_like(value->type)) {
            throw ConstexprError(spec.loc, "'alignas' requires an integral constant expression");
        }
        long long raw = as_integer(value, spec.loc);
        if (raw < 0) {
            throw ConstexprError(spec.loc, "'alignas' requires a non-negative alignment value");
        }
        return static_cast<std::uint64_t>(raw);
    }

    [[nodiscard]] std::uint64_t resolve_alignment_specs(const std::vector<AlignmentSpecifier>& specs,
                                                        std::uint64_t natural_alignment, const SourceLocation& loc,
                                                        std::string_view what) {
        std::uint64_t strictest = 0;
        for (const AlignmentSpecifier& spec : specs) {
            std::uint64_t requested = evaluate_alignment_operand(spec);
            if (requested == 0) continue;
            if (!is_power_of_two(requested)) {
                throw ConstexprError(spec.loc, "'alignas' requires a positive power-of-two alignment");
            }
            if (requested < natural_alignment) {
                throw ConstexprError(spec.loc,
                                     "'alignas' requests alignment " + std::to_string(requested) +
                                         ", which is less strict than the natural alignment " +
                                         std::to_string(natural_alignment) + " of " + std::string(what));
            }
            strictest = std::max(strictest, requested);
        }
        static_cast<void>(loc);
        return strictest > natural_alignment ? strictest : 0;
    }

    void validate_constexpr_stmt_tree(Stmt& stmt) {
        tick(stmt.loc, "checking a constexpr local declaration");
        switch (stmt.kind) {
            case StmtKind::VarDecl:
                // ch05 §9.4: a local variable's own array bound (e.g.
                // `char buf[sizeof(int)];`) must be resolved before
                // anything below reads `stmt.type`'s layout (its
                // `alignas`, if any) or codegen ever sees this
                // declaration.
                resolve_array_bounds_in_type(stmt.type);
                if (!stmt.alignment_specs.empty()) {
                    std::optional<TypeLayoutInfo> layout = layout_of_type(program_, stmt.type);
                    if (!layout.has_value()) {
                        throw ConstexprError(stmt.loc, "cannot apply 'alignas' to this variable type in this version");
                    }
                    stmt.resolved_alignment =
                        resolve_alignment_specs(stmt.alignment_specs, layout->abi_align_bytes, stmt.loc,
                                                "variable '" + stmt.var_name + "'");
                }
                if (stmt.is_constexpr) {
                    execute_stmt(stmt, named_type("void"));
                    if (stmt.init) {
                        const auto& binding = lookup_binding(stmt.var_name, stmt.loc);
                        try {
                            rewrite_expr_as_constant(*stmt.init, binding.cell);
                        } catch (const ConstexprError&) {
                            // Some valid constant-expression results (notably
                            // richer object values) still cannot be lowered
                            // back into source-form AST here. Validation has
                            // already succeeded, so keep the original
                            // initializer in those cases.
                        }
                    }
                } else if (stmt.is_const && (stmt.init || stmt.has_ctor_args)) {
                    try {
                        execute_stmt(stmt, named_type("void"));
                    } catch (const ConstexprError&) {
                    }
                }
                return;
            case StmtKind::Block:
                frames_.push_back({});
                try {
                    for (StmtPtr& nested : stmt.statements) validate_constexpr_stmt_tree(*nested);
                } catch (...) {
                    frames_.pop_back();
                    throw;
                }
                frames_.pop_back();
                return;
            case StmtKind::If:
                if (stmt.then_branch) {
                    frames_.push_back({});
                    try {
                        validate_constexpr_stmt_tree(*stmt.then_branch);
                    } catch (...) {
                        frames_.pop_back();
                        throw;
                    }
                    frames_.pop_back();
                }
                if (stmt.else_branch) {
                    frames_.push_back({});
                    try {
                        validate_constexpr_stmt_tree(*stmt.else_branch);
                    } catch (...) {
                        frames_.pop_back();
                        throw;
                    }
                    frames_.pop_back();
                }
                return;
            case StmtKind::While:
                if (stmt.then_branch) {
                    frames_.push_back({});
                    try {
                        validate_constexpr_stmt_tree(*stmt.then_branch);
                    } catch (...) {
                        frames_.pop_back();
                        throw;
                    }
                    frames_.pop_back();
                }
                return;
            case StmtKind::Return:
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::ExprStmt:
                return;
        }
    }

    [[nodiscard]] std::shared_ptr<Cell> clone_cell(const std::shared_ptr<Cell>& cell) {
        auto copy = std::make_shared<Cell>();
        copy->type = cell->type;
        std::visit(
            [&](const auto& data) {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, long long> ||
                              std::is_same_v<T, double> || std::is_same_v<T, bool> ||
                              std::is_same_v<T, PointerValue> || std::is_same_v<T, SpanValue>) {
                    copy->data = data;
                } else if constexpr (std::is_same_v<T, ObjectValue>) {
                    ObjectValue object_copy;
                    object_copy.type_name = data.type_name;
                    for (const auto& [name, field] : data.fields) object_copy.fields.emplace(name, clone_cell(field));
                    copy->data = std::move(object_copy);
                } else if constexpr (std::is_same_v<T, ArrayValue>) {
                    ArrayValue array_copy;
                    array_copy.element_type = data.element_type;
                    for (const auto& element : data.elements) array_copy.elements.push_back(clone_cell(element));
                    copy->data = std::move(array_copy);
                }
            },
            cell->data);
        return copy;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_scalar_cell(Type type, long long value) {
        auto cell = std::make_shared<Cell>();
        cell->type = std::move(type);
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_double_cell(double value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("double");
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_bool_cell(bool value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("bool");
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::vector<ClassField> collect_class_fields(const ClassDef& def) {
        std::vector<ClassField> fields;
        if (const BaseSpecifier* base = def.direct_ordinary_base()) {
            auto base_it = classes_by_name_.find(base->base_type.name);
            if (base_it == classes_by_name_.end()) {
                throw ConstexprError({}, "missing constexpr class definition for base class '" + base->base_type.name + "'");
            }
            std::vector<ClassField> base_fields = collect_class_fields(*base_it->second);
            fields.insert(fields.end(), base_fields.begin(), base_fields.end());
        }
        fields.insert(fields.end(), def.fields.begin(), def.fields.end());
        return fields;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_default_cell(const Type& type, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        switch (type.kind) {
            case TypeKind::Named:
                if (is_integral_named_type(type.name) && type.name != "bool") {
                    cell->data = 0LL;
                    return cell;
                }
                if (type.name == "bool") {
                    cell->data = false;
                    return cell;
                }
                if (is_floating_like(type)) {
                    cell->data = 0.0;
                    return cell;
                }
                if (type.name == "void") {
                    cell->data = std::monostate{};
                    return cell;
                }
                if (auto struct_it = structs_by_name_.find(type.name); struct_it != structs_by_name_.end()) {
                    ObjectValue object;
                    object.type_name = type.name;
                    for (const StructField& field : struct_it->second->fields) {
                        object.fields.emplace(field.name, make_default_cell(field.type, loc));
                    }
                    cell->data = std::move(object);
                    return cell;
                }
                if (auto class_it = classes_by_name_.find(type.name); class_it != classes_by_name_.end()) {
                    ObjectValue object;
                    object.type_name = type.name;
                    for (const ClassField& field : collect_class_fields(*class_it->second)) {
                        if (field.type.kind == TypeKind::Reference && field.type.pointee) {
                            object.fields.emplace(field.name, make_default_cell(*field.type.pointee, loc));
                        } else {
                            object.fields.emplace(field.name, make_default_cell(field.type, loc));
                        }
                    }
                    cell->data = std::move(object);
                    return cell;
                }
                throw ConstexprError(loc, "type '" + type.name + "' is not constexpr-compatible in Phase D1");
            case TypeKind::Pointer:
                cell->data = PointerValue{};
                return cell;
            case TypeKind::Array: {
                if (!type.element) throw ConstexprError(loc, "malformed array type in constexpr evaluator");
                ArrayValue array;
                array.element_type = *type.element;
                for (long long i = 0; i < type.array_size; ++i) array.elements.push_back(make_default_cell(*type.element, loc));
                cell->data = std::move(array);
                return cell;
            }
            case TypeKind::Reference:
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                throw ConstexprError(loc, "type is not yet supported by the constexpr evaluator in Phase D1");
            case TypeKind::Span:
                if (type.is_mutable_ref) {
                    throw ConstexprError(loc, "mutable std::span<T> is not supported during constant evaluation");
                }
                cell->data = SpanValue{};
                return cell;
        }
        throw ConstexprError(loc, "unsupported constexpr type");
    }

    [[nodiscard]] Binding lookup_binding(const std::string& name, const SourceLocation& loc) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            auto binding_it = it->find(name);
            if (binding_it != it->end()) return binding_it->second;
        }
        if (std::shared_ptr<Cell> global_value = resolve_global_constant(name, loc)) {
            return Binding{global_value, /*read_only=*/true};
        }
        throw ConstexprError(loc, "expression is not a constant expression: identifier '" + name + "' is not available");
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
    [[nodiscard]] std::shared_ptr<Cell> resolve_global_constant(const std::string& name, const SourceLocation& loc) {
        if (auto cached = resolved_global_constants_.find(name); cached != resolved_global_constants_.end()) {
            return cached->second;
        }
        auto global_it = globals_by_name_.find(name);
        if (global_it == globals_by_name_.end()) return nullptr;
        const GlobalVar& global = *global_it->second;
        if (global.decl == nullptr || !global.decl->is_constexpr || !global.decl->init) return nullptr;
        if (globals_resolving_.contains(name)) {
            throw ConstexprError(loc, "constant expression circularly depends on global constexpr variable '" + name + "'");
        }
        globals_resolving_.insert(name);
        std::vector<std::unordered_map<std::string, Binding>> saved_frames = std::move(frames_);
        frames_.clear();
        std::shared_ptr<Cell> value;
        try {
            value = evaluate_expr(*global.decl->init);
        } catch (...) {
            frames_ = std::move(saved_frames);
            globals_resolving_.erase(name);
            throw;
        }
        frames_ = std::move(saved_frames);
        globals_resolving_.erase(name);
        resolved_global_constants_.emplace(name, value);
        return value;
    }

    [[nodiscard]] long long as_integer(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (!is_integer_like(cell->type)) {
            throw ConstexprError(loc, "expected an integer-like constexpr value");
        }
        if (is_named_type(cell->type, "bool")) return std::get<bool>(cell->data) ? 1LL : 0LL;
        return std::get<long long>(cell->data);
    }

    [[nodiscard]] double as_double(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_floating_like(cell->type)) return std::get<double>(cell->data);
        if (is_integer_like(cell->type)) return static_cast<double>(as_integer(cell, loc));
        throw ConstexprError(loc, "expected a numeric constexpr value");
    }

    [[nodiscard]] bool as_bool(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_named_type(cell->type, "bool")) return std::get<bool>(cell->data);
        if (is_integer_like(cell->type)) return as_integer(cell, loc) != 0;
        throw ConstexprError(loc, "expected a boolean constexpr value");
    }

    [[nodiscard]] std::pair<long long, long long> integer_bounds_for_type(const Type& type) const {
        if (is_named_type(type, "char")) return {0, 255};
        if (is_named_type(type, "bool")) return {0, 1};
        if (is_named_type(type, "int")) {
            return {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()};
        }
        if (is_named_type(type, "int8_t")) return {-128, 127};
        if (is_named_type(type, "uint8_t")) return {0, 255};
        if (is_named_type(type, "int16_t")) return {-32768, 32767};
        if (is_named_type(type, "uint16_t")) return {0, 65535};
        if (is_named_type(type, "int32_t")) {
            return {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()};
        }
        if (is_named_type(type, "unsigned int")) return {0, std::numeric_limits<std::uint32_t>::max()};
        if (is_named_type(type, "size_t") || is_named_type(type, "uint64_t") || is_named_type(type, "unsigned long")) {
            return {0, std::numeric_limits<long long>::max()};
        }
        if (is_named_type(type, "ptrdiff_t") || is_named_type(type, "int64_t") || is_named_type(type, "long")) {
            return {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()};
        }
        return {std::numeric_limits<long long>::min(), std::numeric_limits<long long>::max()};
    }

    void checked_assign_integer(const std::shared_ptr<Cell>& target, long long value, const SourceLocation& loc) {
        if (is_named_type(target->type, "bool")) {
            target->data = (value != 0);
            return;
        }
        auto [min_value, max_value] = integer_bounds_for_type(target->type);
        if (value < min_value || value > max_value) {
            throw ConstexprError(loc, "constexpr integer overflow");
        }
        target->data = value;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_checked_int_cell(long long value, const SourceLocation& loc) {
        return make_checked_int_cell_as(named_type("int"), value, loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> make_checked_int_cell_as(const Type& type, long long value, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        checked_assign_integer(cell, value, loc);
        return cell;
    }

    void copy_into(const std::shared_ptr<Cell>& target, const std::shared_ptr<Cell>& source, const SourceLocation& loc) {
        if (!types_equal(target->type, source->type)) {
            throw ConstexprError(loc, "constexpr assignment requires exactly matching types");
        }
        std::shared_ptr<Cell> cloned = clone_cell(source);
        target->data = std::move(cloned->data);
    }

    [[nodiscard]] std::shared_ptr<Cell> bind_read_only_span(const Type& span_type, const Expr& init_expr,
                                                            const SourceLocation& loc) {
        if (span_type.kind != TypeKind::Span || !span_type.pointee) {
            throw ConstexprError(loc, "malformed constexpr span type");
        }
        if (span_type.is_mutable_ref) {
            throw ConstexprError(loc, "mutable std::span<T> is not supported during constant evaluation");
        }

        auto result = std::make_shared<Cell>();
        result->type = span_type;
        SpanValue span;

        if (init_expr.kind == ExprKind::StringLiteral) {
            std::shared_ptr<Cell> pointer_cell = evaluate_expr(init_expr);
            auto* pointer = std::get_if<PointerValue>(&pointer_cell->data);
            auto* array = pointer && pointer->storage ? std::get_if<ArrayValue>(&pointer->storage->data) : nullptr;
            if (!pointer || !array) {
                throw ConstexprError(loc, "string-literal span binding lost its backing storage");
            }
            if (!types_equal(*span_type.pointee, array->element_type)) {
                throw ConstexprError(loc, "string-literal element type does not match std::span element type");
            }
            span.pointer = *pointer;
            span.size = static_cast<long long>(array->elements.size()) - 1;
            result->data = std::move(span);
            return result;
        }

        LValue source = resolve_lvalue(init_expr);
        auto* array = std::get_if<ArrayValue>(&source.cell->data);
        if (!array) {
            throw ConstexprError(loc, "std::span<const T> can only be constructed from an array or string literal");
        }
        if (!types_equal(*span_type.pointee, array->element_type)) {
            throw ConstexprError(loc, "array element type does not match std::span element type");
        }
        span.pointer.storage = source.cell;
        span.pointer.index = 0;
        span.pointer.storage_id = "span#" + std::to_string(string_storage_counter_ + 1);
        span.size = static_cast<long long>(array->elements.size());
        result->data = std::move(span);
        return result;
    }

    [[nodiscard]] LValue resolve_lvalue(const Expr& expr) {
        tick(expr.loc, "resolving an lvalue");
        switch (expr.kind) {
            case ExprKind::Identifier: {
                Binding binding = lookup_binding(expr.name, expr.loc);
                return LValue{binding.cell, binding.read_only};
            }
            case ExprKind::Member: {
                LValue base = resolve_lvalue(*expr.lhs);
                auto* object = std::get_if<ObjectValue>(&base.cell->data);
                if (!object) throw ConstexprError(expr.loc, "member access requires a constexpr object value");
                auto it = object->fields.find(expr.name);
                if (it == object->fields.end()) {
                    throw ConstexprError(expr.loc, "unknown constexpr field '" + expr.name + "'");
                }
                return LValue{it->second, base.read_only};
            }
            case ExprKind::Subscript: {
                std::shared_ptr<Cell> base;
                bool base_read_only = false;
                try {
                    LValue base_lvalue = resolve_lvalue(*expr.lhs);
                    base = base_lvalue.cell;
                    base_read_only = base_lvalue.read_only;
                } catch (const ConstexprError&) {
                    base = evaluate_expr(*expr.lhs);
                }
                long long index = as_integer(evaluate_expr(*expr.rhs), expr.loc);
                if (auto* array = std::get_if<ArrayValue>(&base->data)) {
                    if (index < 0 || static_cast<std::size_t>(index) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr subscript out of bounds");
                    }
                    return LValue{array->elements[static_cast<std::size_t>(index)], base_read_only};
                }
                if (auto* span = std::get_if<SpanValue>(&base->data)) {
                    if (index < 0 || index >= span->size) {
                        throw ConstexprError(expr.loc, "constexpr span subscript out of bounds");
                    }
                    PointerValue element_ptr = span->pointer;
                    element_ptr.index += index;
                    return LValue{dereference_pointer(element_ptr, make_pointer_type_to(*base->type.pointee, false), expr.loc),
                                  true};
                }
                if (auto* pointer = std::get_if<PointerValue>(&base->data)) {
                    if (!base->type.pointee) throw ConstexprError(expr.loc, "malformed constexpr pointer type");
                    PointerValue shifted = *pointer;
                    shifted.index += index;
                    auto* array = shifted.storage ? std::get_if<ArrayValue>(&shifted.storage->data) : nullptr;
                    if (!array) throw ConstexprError(expr.loc, "constexpr pointer does not point to indexable storage");
                    if (shifted.index < 0 || static_cast<std::size_t>(shifted.index) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr subscript out of bounds");
                    }
                    return LValue{dereference_pointer(shifted, base->type, expr.loc), true};
                }
                throw ConstexprError(expr.loc, "constexpr subscript requires an array, pointer, or std::span");
            }
            case ExprKind::Unary:
                if (expr.unary_op == UnaryOp::Deref) {
                    std::shared_ptr<Cell> pointer_cell = evaluate_expr(*expr.lhs);
                    auto* pointer = std::get_if<PointerValue>(&pointer_cell->data);
                    if (!pointer) throw ConstexprError(expr.loc, "constexpr dereference requires a pointer");
                    return LValue{dereference_pointer(*pointer, pointer_cell->type, expr.loc), true};
                }
                break;
            default:
                break;
        }
        throw ConstexprError(expr.loc, "expression is not an assignable constexpr lvalue");
    }

    [[nodiscard]] std::shared_ptr<Cell> make_string_literal_pointer(const Expr& expr) {
        Type array_type;
        array_type.kind = TypeKind::Array;
        array_type.element = std::make_shared<Type>(named_type("char"));
        array_type.array_size = static_cast<long long>(expr.name.size()) + 1;
        auto storage = std::make_shared<Cell>();
        storage->type = array_type;
        ArrayValue array;
        array.element_type = named_type("char");
        for (unsigned char ch : expr.name) array.elements.push_back(make_scalar_cell(named_type("char"), static_cast<long long>(ch)));
        array.elements.push_back(make_scalar_cell(named_type("char"), 0));
        storage->data = std::move(array);

        auto result = std::make_shared<Cell>();
        result->type = make_const_char_pointer_type();
        PointerValue pointer;
        pointer.storage = storage;
        pointer.storage_id = "string#" + std::to_string(++string_storage_counter_);
        result->data = std::move(pointer);
        return result;
    }

    [[nodiscard]] const Function* find_callable(std::string_view name, const std::vector<std::shared_ptr<Cell>>& args,
                                                bool require_constexpr) {
        auto it = functions_by_name_.find(std::string(name));
        if (it == functions_by_name_.end()) return nullptr;
        for (std::size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn->params[i].type, args[i], require_constexpr)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return fn;
        }
        return nullptr;
    }

    [[nodiscard]] const Function* find_single_argument_converting_constructor(std::string_view class_name,
                                                                              const std::shared_ptr<Cell>& arg,
                                                                              bool require_constexpr) {
        auto it = functions_by_name_.find(std::string(class_name) + "_new");
        if (it == functions_by_name_.end()) return nullptr;
        for (std::size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != 2) continue;
            const Type& param_type = fn->params[1].type;
            const Type& arg_type = arg->type;
            if (param_type.kind == TypeKind::Reference) {
                if (param_type.pointee && types_equal(*param_type.pointee, arg_type)) return fn;
            } else if (types_equal(param_type, arg_type)) {
                return fn;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool is_same_or_base_class_type(const Type& expected, const Type& actual) const {
        if (types_equal(expected, actual)) return true;
        if (expected.kind != TypeKind::Named || actual.kind != TypeKind::Named) return false;
        if (!is_class_name(expected.name) || !is_class_name(actual.name)) return false;
        std::string current = actual.name;
        while (true) {
            auto it = classes_by_name_.find(current);
            if (it == classes_by_name_.end()) return false;
            const BaseSpecifier* base = it->second->direct_ordinary_base();
            if (base == nullptr) return false;
            current = base->base_type.name;
            if (current == expected.name) return true;
        }
    }

    [[nodiscard]] std::shared_ptr<Cell> clone_cell_as_type(const std::shared_ptr<Cell>& cell, const Type& target_type,
                                                           const SourceLocation& loc) {
        auto clone = clone_cell(cell);
        if (!is_same_or_base_class_type(target_type, clone->type)) {
            throw ConstexprError(loc, "constexpr value is not compatible with requested parameter type");
        }
        clone->type = target_type;
        if (auto* object = std::get_if<ObjectValue>(&clone->data)) object->type_name = target_type.name;
        return clone;
    }

    [[nodiscard]] std::shared_ptr<Cell> alias_cell_as_type(const std::shared_ptr<Cell>& cell, const Type& target_type,
                                                           const SourceLocation& loc) {
        if (!is_same_or_base_class_type(target_type, cell->type)) {
            throw ConstexprError(loc, "constexpr object is not compatible with requested reference type");
        }
        auto* object = std::get_if<ObjectValue>(&cell->data);
        if (!object) throw ConstexprError(loc, "constexpr base-class binding requires an object value");
        auto alias = std::make_shared<Cell>();
        alias->type = target_type;
        ObjectValue alias_object;
        alias_object.type_name = target_type.name;
        for (const auto& [name, field] : object->fields) alias_object.fields.emplace(name, field);
        alias->data = std::move(alias_object);
        return alias;
    }

    [[nodiscard]] bool constexpr_argument_matches_parameter(const Type& param_type, const std::shared_ptr<Cell>& arg,
                                                            bool require_constexpr) {
        const Type& arg_type = arg->type;
        if (param_type.kind == TypeKind::Reference) {
            return param_type.pointee && is_same_or_base_class_type(*param_type.pointee, arg_type);
        }
        if (is_same_or_base_class_type(param_type, arg_type)) return true;
        if (param_type.kind == TypeKind::Named && is_class_name(param_type.name)) {
            return find_single_argument_converting_constructor(param_type.name, arg, require_constexpr) != nullptr;
        }
        return false;
    }

    [[nodiscard]] const Function* find_constructor(std::string_view class_name,
                                                   const std::vector<std::shared_ptr<Cell>>& args,
                                                   bool require_constexpr) {
        auto it = functions_by_name_.find(std::string(class_name) + "_new");
        if (it == functions_by_name_.end()) return nullptr;
        for (std::size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != args.size() + 1) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                const Type& param_type = fn->params[i + 1].type;
                const Type& arg_type = args[i]->type;
                if (param_type.kind == TypeKind::Reference) {
                    if (!param_type.pointee || !types_equal(*param_type.pointee, arg_type)) {
                        params_match = false;
                        break;
                    }
                } else if (!types_equal(param_type, arg_type)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return fn;
        }
        return nullptr;
    }

    [[nodiscard]] bool has_runtime_only_match(std::string_view name, const std::vector<std::shared_ptr<Cell>>& args) {
        auto it = functions_by_name_.find(std::string(name));
        if (it == functions_by_name_.end()) return false;
        for (std::size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body || fn->eval_mode != FunctionEvalMode::RuntimeOnly || fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn->params[i].type, args[i], /*require_constexpr=*/false)) {
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

    void apply_default_initializers_to_named_object(const std::shared_ptr<Cell>& object_cell, const Type& object_type,
                                                    const SourceLocation& loc) {
        if (object_type.kind != TypeKind::Named) return;
        auto* object = std::get_if<ObjectValue>(&object_cell->data);
        if (!object) return;
        if (auto struct_it = structs_by_name_.find(object_type.name); struct_it != structs_by_name_.end()) {
            for (const StructField& field : struct_it->second->fields) {
                if (!field.default_initializer) continue;
                auto field_it = object->fields.find(field.name);
                if (field_it == object->fields.end()) continue;
                apply_initializer_to_field(field_it->second, field.type, *field.default_initializer, loc);
            }
            return;
        }
        if (auto class_it = classes_by_name_.find(object_type.name); class_it != classes_by_name_.end()) {
            for (const ClassField& field : collect_class_fields(*class_it->second)) {
                if (!field.default_initializer) continue;
                auto field_it = object->fields.find(field.name);
                if (field_it == object->fields.end()) continue;
                apply_initializer_to_field(field_it->second, field.type, *field.default_initializer, loc);
            }
        }
    }

    void apply_initializer_to_field(std::shared_ptr<Cell>& field_cell, const Type& field_type, const Initializer& init,
                                    const SourceLocation& loc) {
        if (field_type.kind == TypeKind::Reference) {
            const Expr* ref_expr = init.expr.get();
            if (init.has_brace_args) {
                if (init.brace_args.size() != 1) {
                    throw ConstexprError(loc, "a reference member must be initialized with exactly one expression");
                }
                ref_expr = init.brace_args[0].get();
            }
            if (ref_expr == nullptr) throw ConstexprError(loc, "a reference member must be initialized");
            if (field_type.is_mutable_ref) {
                field_cell = resolve_lvalue(*ref_expr).cell;
            } else {
                try {
                    field_cell = resolve_lvalue(*ref_expr).cell;
                } catch (const ConstexprError&) {
                    field_cell = evaluate_expr(*ref_expr);
                }
            }
            return;
        }
        if (field_type.kind == TypeKind::Span) {
            const Expr* span_expr = init.expr.get();
            if (init.has_brace_args) {
                if (init.brace_args.size() != 1) {
                    throw ConstexprError(loc, "a span member must be initialized with exactly one array expression");
                }
                span_expr = init.brace_args[0].get();
            }
            if (span_expr == nullptr) throw ConstexprError(loc, "a span member must be initialized");
            field_cell = bind_read_only_span(field_type, *span_expr, loc);
            return;
        }
        if (field_type.kind == TypeKind::Named &&
            (is_class_name(field_type.name) || structs_by_name_.contains(field_type.name)) && init.has_brace_args) {
            std::vector<std::shared_ptr<Cell>> arg_values;
            arg_values.reserve(init.brace_args.size());
            for (const ExprPtr& arg : init.brace_args) arg_values.push_back(evaluate_expr(*arg));
            if (const Function* ctor = find_constructor(field_type.name, arg_values, /*require_constexpr=*/true)) {
                std::vector<Binding> bindings;
                bindings.reserve(ctor->params.size());
                bindings.push_back(Binding{field_cell, false});
                for (std::size_t i = 1; i < ctor->params.size(); ++i) {
                    const Param& param = ctor->params[i];
                    const Expr& arg_expr = *init.brace_args[i - 1];
                    if (param.type.kind == TypeKind::Reference) {
                        if (param.type.is_rvalue_ref) {
                            bindings.push_back(Binding{evaluate_expr(arg_expr), false});
                        } else if (param.type.is_mutable_ref) {
                            bindings.push_back(Binding{resolve_lvalue(arg_expr).cell, false});
                        } else {
                            try {
                                bindings.push_back(Binding{resolve_lvalue(arg_expr).cell, true});
                            } catch (const ConstexprError&) {
                                bindings.push_back(Binding{evaluate_expr(arg_expr), true});
                            }
                        }
                    } else {
                        bindings.push_back(Binding{evaluate_expr(arg_expr), false});
                    }
                }
                static_cast<void>(call_function(*ctor, std::move(bindings), loc));
                return;
            }
            if (init.brace_args.empty()) {
                apply_default_initializers_to_named_object(field_cell, field_type, loc);
                return;
            }
        }
        if (init.has_brace_args) {
            if (init.brace_args.empty()) return;
            if (init.brace_args.size() != 1) {
                throw ConstexprError(loc, "brace-initialization of this member requires exactly one expression");
            }
            copy_into(field_cell, evaluate_expr(*init.brace_args[0]), loc);
            return;
        }
        if (init.expr) copy_into(field_cell, evaluate_expr(*init.expr), loc);
    }

    void execute_constructor_member_initializers(const Function& fn) {
        if (!is_constructor_function(fn)) return;
        Binding this_binding = lookup_binding("this", fn.loc);
        auto* object = std::get_if<ObjectValue>(&this_binding.cell->data);
        if (!object) throw ConstexprError(fn.loc, "constructor receiver is not an object during constant evaluation");
        if (auto struct_it = structs_by_name_.find(fn.member_owner_class); struct_it != structs_by_name_.end()) {
            for (const StructField& field : struct_it->second->fields) {
                const Initializer* selected = nullptr;
                for (const MemberInitializer& init : fn.member_initializers) {
                    if (init.member_name == field.name) {
                        selected = &init.initializer;
                        break;
                    }
                }
                if (selected == nullptr && field.default_initializer) selected = &*field.default_initializer;
                if (selected == nullptr) continue;
                auto field_it = object->fields.find(field.name);
                if (field_it == object->fields.end()) {
                    throw ConstexprError(fn.loc, "missing constexpr storage for field '" + field.name + "'");
                }
                apply_initializer_to_field(field_it->second, field.type, *selected, fn.loc);
            }
            return;
        }
        auto class_it = classes_by_name_.find(fn.member_owner_class);
        if (class_it == classes_by_name_.end()) {
            throw ConstexprError(fn.loc, "missing constexpr class definition for '" + fn.member_owner_class + "'");
        }
        for (const ClassField& field : class_it->second->fields) {
            const Initializer* selected = nullptr;
            for (const MemberInitializer& init : fn.member_initializers) {
                if (init.member_name == field.name) {
                    selected = &init.initializer;
                    break;
                }
            }
            if (selected == nullptr && field.default_initializer) selected = &*field.default_initializer;
            if (selected == nullptr) continue;
            auto field_it = object->fields.find(field.name);
            if (field_it == object->fields.end()) {
                throw ConstexprError(fn.loc, "missing constexpr storage for field '" + field.name + "'");
            }
            apply_initializer_to_field(field_it->second, field.type, *selected, fn.loc);
        }
    }

    [[nodiscard]] bool is_class_name(std::string_view name) const {
        return classes_by_name_.contains(std::string(name));
    }

    [[nodiscard]] bool has_user_defined_destructor(std::string_view class_name) const {
        auto it = functions_by_name_.find(std::string(class_name) + "_delete");
        if (it == functions_by_name_.end()) return false;
        for (std::size_t fn_index : it->second) {
            if (program_.functions[fn_index].body) return true;
        }
        return false;
    }

    void reject_user_defined_destructor_execution(const Type& type, const SourceLocation& loc) const {
        if (type.kind != TypeKind::Named || !is_class_name(type.name) || !has_user_defined_destructor(type.name)) return;
        throw ConstexprError(loc, "required constant evaluation cannot execute user-defined destructor of '" + type.name +
                                      "'");
    }

    [[nodiscard]] std::shared_ptr<Cell> cast_value(const Type& target_type, const std::shared_ptr<Cell>& operand,
                                                   const SourceLocation& loc) {
        if (is_named_type(target_type, "double")) return make_double_cell(as_double(operand, loc));
        if (is_named_type(target_type, "bool")) return make_bool_cell(as_bool(operand, loc));
        if (is_named_type(target_type, "int") || is_named_type(target_type, "char")) {
            auto result = std::make_shared<Cell>();
            result->type = target_type;
            checked_assign_integer(result, static_cast<long long>(as_double(operand, loc)), loc);
            return result;
        }
        throw ConstexprError(loc, "constexpr cast only supports builtin scalar targets in Phase D1");
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_binary_numeric(const Expr& expr, const std::shared_ptr<Cell>& lhs,
                                                                const std::shared_ptr<Cell>& rhs) {
        if (is_floating_like(lhs->type) || is_floating_like(rhs->type)) {
            double left = as_double(lhs, expr.loc);
            double right = as_double(rhs, expr.loc);
            switch (expr.binary_op) {
                case BinaryOp::Add: return make_double_cell(left + right);
                case BinaryOp::Sub: return make_double_cell(left - right);
                case BinaryOp::Mul: return make_double_cell(left * right);
                case BinaryOp::Div: return make_double_cell(left / right);
                case BinaryOp::Eq: return make_bool_cell(left == right);
                case BinaryOp::Ne: return make_bool_cell(left != right);
                case BinaryOp::Lt: return make_bool_cell(left < right);
                case BinaryOp::Gt: return make_bool_cell(left > right);
                case BinaryOp::Le: return make_bool_cell(left <= right);
                case BinaryOp::Ge: return make_bool_cell(left >= right);
                default: break;
            }
        } else {
            long long left = as_integer(lhs, expr.loc);
            long long right = as_integer(rhs, expr.loc);
            Type result_type = types_equal(lhs->type, rhs->type) ? lhs->type : named_type("int");
            switch (expr.binary_op) {
                case BinaryOp::Add: {
                    long long result;
                    if (__builtin_add_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Sub: {
                    long long result;
                    if (__builtin_sub_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Mul: {
                    long long result;
                    if (__builtin_mul_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell_as(result_type, result, expr.loc);
                }
                case BinaryOp::Div:
                    if (right == 0) throw ConstexprError(expr.loc, "constexpr division by zero");
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
        throw ConstexprError(expr.loc, "unsupported constexpr binary operator");
    }

    [[nodiscard]] std::shared_ptr<Cell> call_function(const Function& fn, std::vector<Binding> bindings,
                                                      const SourceLocation& loc) {
        tick(loc, "calling an immediate function");
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly) {
            throw ConstexprError(loc, "immediate evaluation may only call constexpr/consteval functions");
        }
        if (!fn.body) throw ConstexprError(loc, "cannot evaluate a declaration-only function at compile time");
        ++call_depth_;
        if (call_depth_ > limits_.max_recursion_depth) {
            --call_depth_;
            throw ConstexprError(loc, "constexpr evaluation exceeded recursion budget");
        }
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (fn.params[i].type.kind == TypeKind::Reference) continue;
            reject_user_defined_destructor_execution(fn.params[i].type, loc);
        }
        frames_.push_back({});
        auto& frame = frames_.back();
        for (std::size_t i = 0; i < fn.params.size(); ++i) frame.emplace(fn.params[i].name, std::move(bindings[i]));
        try {
            execute_constructor_member_initializers(fn);
            execute_stmt(*fn.body, fn.return_type);
        } catch (const ReturnSignal& signal) {
            frames_.pop_back();
            --call_depth_;
            return signal.value ? clone_cell(signal.value) : make_default_cell(fn.return_type, loc);
        }
        frames_.pop_back();
        --call_depth_;
        if (is_named_type(fn.return_type, "void")) {
            auto result = std::make_shared<Cell>();
            result->type = named_type("void");
            return result;
        }
        return make_default_cell(fn.return_type, loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> call_with_expr_arg_views(const Function& fn, const std::vector<const Expr*>& args,
                                                                 const SourceLocation& loc) {
        std::vector<Binding> bindings;
        bindings.reserve(fn.params.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            const Expr& arg_expr = *args[i];
            if (param.type.kind == TypeKind::Reference) {
                if (param.type.is_rvalue_ref) {
                    std::shared_ptr<Cell> value = evaluate_expr(arg_expr);
                    if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, value->type) &&
                        !types_equal(*param.type.pointee, value->type)) {
                        value = clone_cell_as_type(value, *param.type.pointee, loc);
                    }
                    bindings.push_back(Binding{value, false});
                    continue;
                }
                if (param.type.is_mutable_ref) {
                    LValue arg = resolve_lvalue(arg_expr);
                    if (arg.read_only) {
                        throw ConstexprError(loc, "cannot bind a const/constexpr value to mutable reference parameter '" +
                                                      param.name + "'");
                    }
                    if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, arg.cell->type) &&
                        !types_equal(*param.type.pointee, arg.cell->type)) {
                        bindings.push_back(Binding{alias_cell_as_type(arg.cell, *param.type.pointee, loc), false});
                    } else {
                        bindings.push_back(Binding{arg.cell, false});
                    }
                } else {
                    bool can_bind_lvalue = false;
                    try {
                        LValue arg = resolve_lvalue(arg_expr);
                        if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, arg.cell->type) &&
                            !types_equal(*param.type.pointee, arg.cell->type)) {
                            bindings.push_back(Binding{alias_cell_as_type(arg.cell, *param.type.pointee, loc), true});
                        } else {
                            bindings.push_back(Binding{arg.cell, true});
                        }
                        can_bind_lvalue = true;
                    } catch (const ConstexprError&) {
                    }
                    if (!can_bind_lvalue) {
                        std::shared_ptr<Cell> value = evaluate_expr(arg_expr);
                        if (param.type.pointee && is_same_or_base_class_type(*param.type.pointee, value->type) &&
                            !types_equal(*param.type.pointee, value->type)) {
                            value = clone_cell_as_type(value, *param.type.pointee, loc);
                        }
                        bindings.push_back(Binding{value, true});
                    }
                }
            } else {
                std::shared_ptr<Cell> value = evaluate_expr(arg_expr);
                if (is_same_or_base_class_type(param.type, value->type) && !types_equal(param.type, value->type)) {
                    bindings.push_back(Binding{clone_cell_as_type(value, param.type, loc), false});
                } else if (!types_equal(param.type, value->type) &&
                           param.type.kind == TypeKind::Named && is_class_name(param.type.name)) {
                    const Function* ctor =
                        find_single_argument_converting_constructor(param.type.name, value, /*require_constexpr=*/true);
                    if (ctor == nullptr) {
                        throw ConstexprError(loc, "constexpr call has no viable converting constructor for parameter '" +
                                                      param.name + "'");
                    }
                    auto object = make_default_cell(param.type, loc);
                    std::vector<Binding> ctor_bindings;
                    ctor_bindings.push_back(Binding{object, false});
                    ctor_bindings.push_back(Binding{value, false});
                    (void)call_function(*ctor, std::move(ctor_bindings), loc);
                    bindings.push_back(Binding{object, false});
                } else {
                    bindings.push_back(Binding{value, false});
                }
            }
        }
        return call_function(fn, std::move(bindings), loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> call_with_expr_args(const Function& fn, const std::vector<ExprPtr>& args,
                                                            const SourceLocation& loc) {
        std::vector<const Expr*> arg_views;
        arg_views.reserve(args.size());
        for (const ExprPtr& arg : args) arg_views.push_back(arg.get());
        return call_with_expr_arg_views(fn, arg_views, loc);
    }

    [[nodiscard]] const Function* find_method_callable(const Expr& receiver_expr, std::string_view method_name,
                                                       const std::vector<std::shared_ptr<Cell>>& arg_values,
                                                       bool require_constexpr) {
        std::shared_ptr<Cell> receiver_value;
        bool receiver_is_lvalue = false;
        bool receiver_read_only = false;
        try {
            LValue receiver = resolve_lvalue(receiver_expr);
            receiver_value = receiver.cell;
            receiver_is_lvalue = true;
            receiver_read_only = receiver.read_only;
        } catch (const ConstexprError&) {
            receiver_value = evaluate_expr(receiver_expr);
        }
        if (receiver_value->type.kind != TypeKind::Named || !is_class_name(receiver_value->type.name)) return nullptr;

        std::string full_name = receiver_value->type.name + "_" + std::string(method_name);
        auto it = functions_by_name_.find(full_name);
        if (it == functions_by_name_.end()) return nullptr;
        for (std::size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != arg_values.size() + 1 || fn->params.empty()) continue;

            const Type& this_type = fn->params[0].type;
            if (this_type.kind == TypeKind::Reference) {
                if (!this_type.pointee || !is_same_or_base_class_type(*this_type.pointee, receiver_value->type)) continue;
                if (this_type.is_mutable_ref && (!receiver_is_lvalue || receiver_read_only)) continue;
            } else if (!is_same_or_base_class_type(this_type, receiver_value->type)) {
                continue;
            }

            bool params_match = true;
            for (std::size_t i = 0; i < arg_values.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn->params[i + 1].type, arg_values[i], require_constexpr)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return fn;
        }
        return nullptr;
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_constructor_expr(const Expr& expr) {
        Type object_type = named_type(expr.name);
        auto object = make_default_cell(object_type, expr.loc);
        std::vector<std::shared_ptr<Cell>> arg_values;
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) arg_values.push_back(evaluate_expr(*arg));
        const Function* ctor = find_constructor(expr.name, arg_values, /*require_constexpr=*/true);
        if (!ctor) {
            if (expr.args.empty() &&
                (classes_by_name_.contains(expr.name) || structs_by_name_.contains(expr.name))) {
                apply_default_initializers_to_named_object(object, object_type, expr.loc);
                return object;
            }
            if (has_runtime_only_match(expr.name + "_new", arg_values)) {
                throw ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval constructors");
            }
            throw ConstexprError(expr.loc, "no constexpr/consteval constructor matches for type '" + expr.name + "'");
        }
        std::vector<Binding> bindings;
        bindings.reserve(ctor->params.size());
        bindings.push_back(Binding{object, false});
        for (std::size_t i = 1; i < ctor->params.size(); ++i) {
            const Param& param = ctor->params[i];
            const Expr& arg_expr = *expr.args[i - 1];
            if (param.type.kind == TypeKind::Reference) {
                if (param.type.is_rvalue_ref) {
                    bindings.push_back(Binding{evaluate_expr(arg_expr), false});
                    continue;
                }
                if (param.type.is_mutable_ref) {
                    LValue arg = resolve_lvalue(arg_expr);
                    bindings.push_back(Binding{arg.cell, false});
                } else {
                    bool can_bind_lvalue = false;
                    try {
                        LValue arg = resolve_lvalue(arg_expr);
                        bindings.push_back(Binding{arg.cell, true});
                        can_bind_lvalue = true;
                    } catch (const ConstexprError&) {
                    }
                    if (!can_bind_lvalue) bindings.push_back(Binding{evaluate_expr(arg_expr), true});
                }
            } else {
                bindings.push_back(Binding{evaluate_expr(arg_expr), false});
            }
        }
        static_cast<void>(call_function(*ctor, std::move(bindings), expr.loc));
        return object;
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_call_expr(const Expr& expr) {
        if (expr.lhs) {
            std::vector<std::shared_ptr<Cell>> arg_values;
            arg_values.reserve(expr.args.size());
            for (const ExprPtr& arg : expr.args) arg_values.push_back(evaluate_expr(*arg));
            const Function* fn = find_method_callable(*expr.lhs, expr.name, arg_values, /*require_constexpr=*/true);
            if (!fn) {
                throw ConstexprError(expr.loc,
                                     "no constexpr/consteval overload of method '" + expr.name + "' matches this immediate call");
            }
            std::vector<const Expr*> all_args;
            all_args.reserve(expr.args.size() + 1);
            all_args.push_back(expr.lhs.get());
            for (const ExprPtr& arg : expr.args) all_args.push_back(arg.get());
            return call_with_expr_arg_views(*fn, all_args, expr.loc);
        }
        if (is_class_name(expr.name)) return evaluate_constructor_expr(expr);
        std::vector<std::shared_ptr<Cell>> arg_values;
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) arg_values.push_back(evaluate_expr(*arg));
        const Function* fn = find_callable(expr.name, arg_values, /*require_constexpr=*/true);
        if (!fn) {
            if (has_runtime_only_match(expr.name, arg_values)) {
                throw ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval functions");
            }
            throw ConstexprError(expr.loc, "no constexpr/consteval overload of '" + expr.name + "' matches this immediate call");
        }
        return call_with_expr_args(*fn, expr.args, expr.loc);
    }

    [[nodiscard]] const EnumDef* find_enum_for_variant(std::string_view variant_name) const {
        for (const EnumDef& def : program_.enums) {
            for (const EnumVariant& variant : def.variants) {
                if (variant.name == variant_name) return &def;
            }
        }
        return nullptr;
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
            case ExprKind::Destroy: return named_type("void");
            case ExprKind::StringLiteral: return make_const_char_pointer_type();
            case ExprKind::Identifier:
                try {
                    return lookup_binding(expr.name, expr.loc).cell->type;
                } catch (const ConstexprError&) {
                    if (const EnumDef* def = find_enum_for_variant(expr.name)) return named_type(def->name);
                    return std::nullopt;
                }
            case ExprKind::Move:
                return expr.lhs ? infer_unevaluated_expr_type(*expr.lhs) : std::nullopt;
            case ExprKind::New: {
                Type result;
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
                if (auto* span_pointee = base->kind == TypeKind::Span ? base->pointee.get() : nullptr; span_pointee && expr.name == "size") {
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
                    for (const ClassField& field : collect_class_fields(*class_it->second)) {
                        if (field.name == expr.name) return field.type.kind == TypeKind::Reference ? *field.type.pointee : field.type;
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
                if (expr.binary_op == BinaryOp::Assign) return infer_unevaluated_expr_type(*expr.lhs);
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
                    std::string full_name = receiver_named.name + "_" + expr.name;
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

    [[nodiscard]] std::shared_ptr<Cell> evaluate_expr(const Expr& expr) {
        tick(expr.loc, "evaluating an expression");
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return make_scalar_cell(named_type("int"), expr.int_value);
            case ExprKind::FloatLiteral: return make_double_cell(expr.float_value);
            case ExprKind::BoolLiteral: return make_bool_cell(expr.bool_value);
            case ExprKind::CharLiteral: return make_scalar_cell(named_type("char"), expr.int_value);
            case ExprKind::StringLiteral: return make_string_literal_pointer(expr);
            case ExprKind::Destroy:
                throw ConstexprError(expr.loc, "explicit destructor calls are not supported during constant evaluation");
            case ExprKind::Alignof: {
                reject_if_incomplete(expr.type, expr.loc, "alignof");
                std::optional<TypeLayoutInfo> layout = layout_of_type(program_, expr.type);
                if (!layout.has_value()) throw ConstexprError(expr.loc, "cannot apply 'alignof' to this type in this version");
                return make_scalar_cell(named_type("size_t"), static_cast<long long>(layout->abi_align_bytes));
            }
            case ExprKind::Sizeof: {
                Type queried_type;
                if (expr.sizeof_operand_is_type) {
                    queried_type = expr.type;
                } else {
                    std::optional<Type> inferred = infer_unevaluated_expr_type(*expr.lhs);
                    if (!inferred.has_value()) {
                        throw ConstexprError(expr.loc, "cannot apply 'sizeof' to this expression: its type could not be inferred");
                    }
                    queried_type = *inferred;
                }
                reject_if_incomplete(queried_type, expr.loc, "sizeof");
                std::optional<TypeLayoutInfo> layout = layout_of_type(program_, queried_type);
                if (!layout.has_value()) throw ConstexprError(expr.loc, "cannot apply 'sizeof' to this type in this version");
                return make_scalar_cell(named_type("size_t"), static_cast<long long>(layout->size_bytes));
            }
            case ExprKind::Identifier: return clone_cell(lookup_binding(expr.name, expr.loc).cell);
            case ExprKind::Conditional:
                return as_bool(evaluate_expr(*expr.lhs), expr.loc) ? evaluate_expr(*expr.rhs) : evaluate_expr(*expr.third);
            case ExprKind::Member: {
                std::shared_ptr<Cell> base = evaluate_expr(*expr.lhs);
                if (auto* span = std::get_if<SpanValue>(&base->data); span && expr.name == "size") {
                    return make_checked_int_cell(span->size, expr.loc);
                }
                return clone_cell(resolve_lvalue(expr).cell);
            }
            case ExprKind::Subscript: return clone_cell(resolve_lvalue(expr).cell);
            case ExprKind::Call:
                if (expr.lhs == nullptr && expr.name == "$for_range_size" && expr.args.size() == 1) {
                    std::optional<Type> range_type = infer_unevaluated_expr_type(*expr.args[0]);
                    if (!range_type.has_value()) throw ConstexprError(expr.loc, "cannot determine range-for operand type");
                    const Type& unwrapped = range_type->kind == TypeKind::Reference && range_type->pointee != nullptr
                                                ? *range_type->pointee
                                                : *range_type;
                    if (unwrapped.kind == TypeKind::Array) return make_checked_int_cell(unwrapped.array_size, expr.loc);
                    if (unwrapped.kind == TypeKind::Span) {
                        std::shared_ptr<Cell> value = evaluate_expr(*expr.args[0]);
                        if (auto* span = std::get_if<SpanValue>(&value->data)) {
                            return make_checked_int_cell(span->size, expr.loc);
                        }
                    }
                    throw ConstexprError(expr.loc, "range-for requires a fixed-size array or std::span operand");
                }
                return evaluate_call_expr(expr);
            case ExprKind::Cast: return cast_value(expr.type, evaluate_expr(*expr.lhs), expr.loc);
            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Assign) {
                    LValue target = resolve_lvalue(*expr.lhs);
                    if (target.read_only) throw ConstexprError(expr.loc, "cannot assign through a const/constexpr binding");
                    std::shared_ptr<Cell> value = evaluate_expr(*expr.rhs);
                    copy_into(target.cell, value, expr.loc);
                    return clone_cell(target.cell);
                }
                if (expr.binary_op == BinaryOp::And) {
                    if (!as_bool(evaluate_expr(*expr.lhs), expr.loc)) return make_bool_cell(false);
                    return make_bool_cell(as_bool(evaluate_expr(*expr.rhs), expr.loc));
                }
                if (expr.binary_op == BinaryOp::Or) {
                    if (as_bool(evaluate_expr(*expr.lhs), expr.loc)) return make_bool_cell(true);
                    return make_bool_cell(as_bool(evaluate_expr(*expr.rhs), expr.loc));
                }
                return evaluate_binary_numeric(expr, evaluate_expr(*expr.lhs), evaluate_expr(*expr.rhs));
            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Neg: {
                        std::shared_ptr<Cell> operand = evaluate_expr(*expr.lhs);
                        if (is_floating_like(operand->type)) return make_double_cell(-as_double(operand, expr.loc));
                        long long value = as_integer(operand, expr.loc);
                        if (value == std::numeric_limits<long long>::min()) {
                            throw ConstexprError(expr.loc, "constexpr integer overflow");
                        }
                        return make_checked_int_cell(-value, expr.loc);
                    }
                    case UnaryOp::Not: return make_bool_cell(!as_bool(evaluate_expr(*expr.lhs), expr.loc));
                    case UnaryOp::Deref: return clone_cell(resolve_lvalue(expr).cell);
                    case UnaryOp::AddressOf: {
                        LValue target = resolve_lvalue(*expr.lhs);
                        auto result = std::make_shared<Cell>();
                        result->type = make_pointer_type_to(target.cell->type, !target.read_only);
                        PointerValue pointer;
                        if (expr.lhs->kind == ExprKind::Subscript && expr.lhs->lhs && expr.lhs->rhs) {
                            long long offset = as_integer(evaluate_expr(*expr.lhs->rhs), expr.loc);
                            try {
                                LValue base_lvalue = resolve_lvalue(*expr.lhs->lhs);
                                if (std::holds_alternative<ArrayValue>(base_lvalue.cell->data)) {
                                    pointer.storage = base_lvalue.cell;
                                    pointer.index = offset;
                                } else {
                                    pointer.storage = target.cell;
                                    pointer.index = 0;
                                }
                            } catch (const ConstexprError&) {
                                std::shared_ptr<Cell> base_value = evaluate_expr(*expr.lhs->lhs);
                                if (auto* span = std::get_if<SpanValue>(&base_value->data)) {
                                    pointer = span->pointer;
                                    pointer.index += offset;
                                } else if (auto* base_pointer = std::get_if<PointerValue>(&base_value->data)) {
                                    pointer = *base_pointer;
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
                        result->data = std::move(pointer);
                        return result;
                    }
                }
                break;
            case ExprKind::TypeTrait:
                throw ConstexprError(expr.loc, "constexpr type traits are deferred to a later phase");
            case ExprKind::New:
            case ExprKind::Delete:
            case ExprKind::Move:
            case ExprKind::PackExpansion:
            case ExprKind::Lambda:
            case ExprKind::Fold:
                break;
        }
        throw ConstexprError(expr.loc, "expression kind is not yet supported by the constexpr evaluator in Phase D1");
    }

    void execute_stmt(const Stmt& stmt, const Type& return_type) {
        tick(stmt.loc, "executing a statement");
        switch (stmt.kind) {
            case StmtKind::VarDecl: {
                if (stmt.type.kind == TypeKind::Span) {
                    if (!stmt.init) {
                        throw ConstexprError(stmt.loc, "std::span<const T> must be initialized during constant evaluation");
                    }
                    frames_.back()[stmt.var_name] = Binding{bind_read_only_span(stmt.type, *stmt.init, stmt.loc),
                                                            stmt.is_const || stmt.is_constexpr};
                    return;
                }
                reject_user_defined_destructor_execution(stmt.type, stmt.loc);
                auto cell = make_default_cell(stmt.type, stmt.loc);
                if (stmt.has_ctor_args) {
                    std::vector<Binding> ctor_bindings;
                    ctor_bindings.reserve(stmt.ctor_args.size() + 1);
                    ctor_bindings.push_back(Binding{cell, false});
                    std::vector<std::shared_ptr<Cell>> arg_values;
                    arg_values.reserve(stmt.ctor_args.size() + 1);
                    arg_values.push_back(cell);
                    for (const ExprPtr& arg : stmt.ctor_args) arg_values.push_back(evaluate_expr(*arg));
                    const Function* ctor = find_callable(stmt.type.name + "_new", arg_values, /*require_constexpr=*/true);
                    if (!ctor) {
                        if (stmt.ctor_args.empty() &&
                            (classes_by_name_.contains(stmt.type.name) || structs_by_name_.contains(stmt.type.name))) {
                            apply_default_initializers_to_named_object(cell, stmt.type, stmt.loc);
                            frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                            return;
                        }
                        throw ConstexprError(stmt.loc, "no constexpr/consteval constructor matches for type '" + stmt.type.name + "'");
                    }
                    for (std::size_t i = 1; i < ctor->params.size(); ++i) {
                        const Param& param = ctor->params[i];
                        const Expr& arg_expr = *stmt.ctor_args[i - 1];
                        if (param.type.kind == TypeKind::Reference && !param.type.is_rvalue_ref && param.type.is_mutable_ref) {
                            LValue arg = resolve_lvalue(arg_expr);
                            ctor_bindings.push_back(Binding{arg.cell, false});
                        } else {
                            ctor_bindings.push_back(Binding{evaluate_expr(arg_expr), param.type.kind == TypeKind::Reference && !param.type.is_mutable_ref});
                        }
                    }
                    static_cast<void>(call_function(*ctor, std::move(ctor_bindings), stmt.loc));
                } else if (stmt.init) {
                    copy_into(cell, evaluate_expr(*stmt.init), stmt.loc);
                }
                frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                return;
            }
            case StmtKind::Return:
                if (stmt.expr) throw ReturnSignal{evaluate_expr(*stmt.expr)};
                throw ReturnSignal{is_named_type(return_type, "void") ? std::shared_ptr<Cell>() : make_default_cell(return_type, stmt.loc)};
            case StmtKind::ExprStmt:
                if (stmt.expr) static_cast<void>(evaluate_expr(*stmt.expr));
                return;
            case StmtKind::If:
                if (stmt.if_mode == IfMode::ConstevalTrue) {
                    execute_stmt(*stmt.then_branch, return_type);
                    return;
                }
                if (stmt.if_mode == IfMode::ConstevalFalse) {
                    if (stmt.else_branch) execute_stmt(*stmt.else_branch, return_type);
                    return;
                }
                if (as_bool(evaluate_expr(*stmt.condition), stmt.loc)) {
                    execute_stmt(*stmt.then_branch, return_type);
                } else if (stmt.else_branch) {
                    execute_stmt(*stmt.else_branch, return_type);
                }
                return;
            case StmtKind::While: {
                int iterations = 0;
                while (as_bool(evaluate_expr(*stmt.condition), stmt.loc)) {
                    ++iterations;
                    if (iterations > limits_.max_loop_iterations) {
                        throw ConstexprError(stmt.loc, "constexpr evaluation exceeded loop-iteration budget");
                    }
                    try {
                        execute_stmt(*stmt.then_branch, return_type);
                    } catch (const ContinueSignal&) {
                        continue;
                    } catch (const BreakSignal&) {
                        break;
                    }
                }
                return;
            }
            case StmtKind::Break: throw BreakSignal{};
            case StmtKind::Continue: throw ContinueSignal{};
            case StmtKind::Block:
                if (stmt.is_unsafe) throw ConstexprError(stmt.loc, "unsafe blocks are not allowed in constant evaluation");
                frames_.push_back({});
                try {
                    for (const StmtPtr& nested : stmt.statements) execute_stmt(*nested, return_type);
                } catch (...) {
                    frames_.pop_back();
                    throw;
                }
                frames_.pop_back();
                return;
        }
    }
};

[[nodiscard]] const Function* find_consteval_function(const Program& program, const Expr& expr) {
    if (expr.kind != ExprKind::Call || expr.lhs) return nullptr;
    const Function* only_match = nullptr;
    for (const Function& fn : program.functions) {
        if (fn.name != expr.name || fn.eval_mode != FunctionEvalMode::Consteval) continue;
        if (fn.params.size() != expr.args.size()) continue;
        if (!only_match) {
            only_match = &fn;
        } else {
            return nullptr;
        }
    }
    return only_match;
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

void rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value) {
    if (is_named_type(value->type, "int")) {
        expr.kind = ExprKind::IntegerLiteral;
        expr.int_value = std::get<long long>(value->data);
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "char")) {
        expr.kind = ExprKind::CharLiteral;
        expr.int_value = std::get<long long>(value->data);
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "bool")) {
        expr.kind = ExprKind::BoolLiteral;
        expr.bool_value = std::get<bool>(value->data);
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.name.clear();
    } else if (is_named_type(value->type, "double")) {
        expr.kind = ExprKind::FloatLiteral;
        expr.float_value = std::get<double>(value->data);
        expr.int_value = 0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (types_equal(value->type, make_const_char_pointer_type())) {
        auto* pointer = std::get_if<PointerValue>(&value->data);
        auto* array = pointer ? std::get_if<ArrayValue>(&pointer->storage->data) : nullptr;
        if (!pointer || !array || pointer->index != 0) {
            throw ConstexprError(expr.loc, "Phase D1 cannot yet lower this constexpr pointer result back into source form");
        }
        expr.kind = ExprKind::StringLiteral;
        expr.name.clear();
        for (const auto& element : array->elements) {
            long long ch = std::get<long long>(element->data);
            if (ch == 0) break;
            expr.name.push_back(static_cast<char>(ch));
        }
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.bool_value = false;
    } else {
        throw ConstexprError(expr.loc, "Phase D1 can only lower scalar and string-literal immediate results");
    }
    expr.lhs.reset();
    expr.rhs.reset();
    expr.third.reset();
    expr.args.clear();
    expr.explicit_template_args.clear();
    expr.type = value->type;
}

[[nodiscard]] ConstexprValue snapshot_constexpr_value(const std::shared_ptr<Cell>& value, const SourceLocation& loc) {
    ConstexprValue snapshot;
    snapshot.type = value->type;
    if (is_named_type(value->type, "void")) {
        snapshot.kind = ConstexprValueKind::Void;
        return snapshot;
    }
    if (is_named_type(value->type, "int") || is_named_type(value->type, "char")) {
        snapshot.kind = ConstexprValueKind::Integer;
        snapshot.int_value = std::get<long long>(value->data);
        return snapshot;
    }
    if (is_named_type(value->type, "bool")) {
        snapshot.kind = ConstexprValueKind::Bool;
        snapshot.bool_value = std::get<bool>(value->data);
        return snapshot;
    }
    if (is_named_type(value->type, "double")) {
        snapshot.kind = ConstexprValueKind::Double;
        snapshot.double_value = std::get<double>(value->data);
        return snapshot;
    }
    if (types_equal(value->type, make_const_char_pointer_type())) {
        auto* pointer = std::get_if<PointerValue>(&value->data);
        auto* array = pointer ? std::get_if<ArrayValue>(&pointer->storage->data) : nullptr;
        if (!pointer || !array) {
            throw ConstexprError(loc, "unsupported constexpr pointer result");
        }
        snapshot.kind = ConstexprValueKind::StringLiteralPointer;
        for (std::size_t i = static_cast<std::size_t>(std::max(pointer->index, 0LL)); i < array->elements.size(); ++i) {
            long long ch = std::get<long long>(array->elements[i]->data);
            if (ch == 0) break;
            snapshot.string_value.push_back(static_cast<char>(ch));
        }
        return snapshot;
    }
    if (auto* object = std::get_if<ObjectValue>(&value->data)) {
        snapshot.kind = ConstexprValueKind::Object;
        for (const auto& [name, field] : object->fields) {
            snapshot.object_fields.push_back({name, std::make_shared<ConstexprValue>(snapshot_constexpr_value(field, loc))});
        }
        return snapshot;
    }
    if (auto* array = std::get_if<ArrayValue>(&value->data)) {
        snapshot.kind = ConstexprValueKind::Array;
        for (const auto& element : array->elements) {
            snapshot.elements.push_back(snapshot_constexpr_value(element, loc));
        }
        return snapshot;
    }
    throw ConstexprError(loc, "unsupported constexpr value kind");
}

void collect_runtime_expr_rewrites(const Program& program, Expr& expr, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<Stmt*>& consteval_if_rewrites);
void collect_runtime_stmt_rewrites(const Program& program, Stmt& stmt, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<Stmt*>& consteval_if_rewrites);

void collect_runtime_stmt_rewrites(const Program& program, Stmt& stmt, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<Stmt*>& consteval_if_rewrites) {
    if (stmt.kind == StmtKind::If && stmt.if_mode != IfMode::Runtime) {
        Stmt* runtime_branch = nullptr;
        if (stmt.if_mode == IfMode::ConstevalFalse) {
            runtime_branch = stmt.then_branch.get();
        } else if (stmt.else_branch) {
            runtime_branch = stmt.else_branch.get();
        }
        if (runtime_branch) collect_runtime_stmt_rewrites(program, *runtime_branch, engine, expr_rewrites, consteval_if_rewrites);
        consteval_if_rewrites.push_back(&stmt);
        return;
    }

    if (stmt.init) collect_runtime_expr_rewrites(program, *stmt.init, engine, expr_rewrites, consteval_if_rewrites);
    for (ExprPtr& arg : stmt.ctor_args) {
        collect_runtime_expr_rewrites(program, *arg, engine, expr_rewrites, consteval_if_rewrites);
    }
    if (stmt.expr) collect_runtime_expr_rewrites(program, *stmt.expr, engine, expr_rewrites, consteval_if_rewrites);
    if (stmt.condition) {
        collect_runtime_expr_rewrites(program, *stmt.condition, engine, expr_rewrites, consteval_if_rewrites);
    }
    if (stmt.then_branch) collect_runtime_stmt_rewrites(program, *stmt.then_branch, engine, expr_rewrites, consteval_if_rewrites);
    if (stmt.else_branch) collect_runtime_stmt_rewrites(program, *stmt.else_branch, engine, expr_rewrites, consteval_if_rewrites);
    for (StmtPtr& nested : stmt.statements) {
        collect_runtime_stmt_rewrites(program, *nested, engine, expr_rewrites, consteval_if_rewrites);
    }
}

void collect_runtime_expr_rewrites(const Program& program, Expr& expr, ConstexprEngine& engine,
                                   std::vector<ExprRewrite>& expr_rewrites,
                                   std::vector<Stmt*>& consteval_if_rewrites) {
    if (find_consteval_function(program, expr) != nullptr && !expr_depends_on_runtime_bindings(expr)) {
        expr_rewrites.push_back(ExprRewrite{&expr, engine.evaluate_root_expr(expr)});
        return;
    }
    if (expr.lhs) collect_runtime_expr_rewrites(program, *expr.lhs, engine, expr_rewrites, consteval_if_rewrites);
    if (expr.rhs) collect_runtime_expr_rewrites(program, *expr.rhs, engine, expr_rewrites, consteval_if_rewrites);
    if (expr.third) {
        collect_runtime_expr_rewrites(program, *expr.third, engine, expr_rewrites, consteval_if_rewrites);
    }
    for (ExprPtr& arg : expr.args) {
        collect_runtime_expr_rewrites(program, *arg, engine, expr_rewrites, consteval_if_rewrites);
    }
    if (expr.lambda_body) {
        collect_runtime_stmt_rewrites(program, *expr.lambda_body, engine, expr_rewrites, consteval_if_rewrites);
    }
}

void rewrite_consteval_if_for_runtime(Stmt& stmt) {
    if (stmt.kind != StmtKind::If || stmt.if_mode == IfMode::Runtime) return;
    SourceLocation loc = stmt.loc;
    IfMode mode = stmt.if_mode;
    StmtPtr selected;
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
    AlignmentResolver(Program& program, ConstexprEngine& engine) : program_(program), engine_(engine) {
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
    void resolve_array_bounds() {
        for (StructDef& def : program_.structs) resolve_struct_array_bounds(def.name);
        for (ClassDef& def : program_.classes) resolve_class_array_bounds(def.name);
        for (GlobalVar& global : program_.globals) {
            if (global.decl == nullptr) continue;
            resolve_array_bounds_type_dependencies(global.decl->type);
        }
        for (Function& fn : program_.functions) {
            if (fn.is_generic_template) continue;
            if (!fn.member_owner_class.empty() && generic_template_owner_names_.contains(fn.member_owner_class)) {
                continue;
            }
            for (Param& param : fn.params) resolve_array_bounds_type_dependencies(param.type);
            resolve_array_bounds_type_dependencies(fn.return_type);
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
                try {
                    resolve_array_bounds_in_stmt(*fn.body);
                } catch (...) {
                    engine_.end_local_array_bound_scope();
                    throw;
                }
                engine_.end_local_array_bound_scope();
            }
        }
    }

    void run() {
        for (StructDef& def : program_.structs) resolve_struct(def.name);
        for (ClassDef& def : program_.classes) resolve_class(def.name);
        for (GlobalVar& global : program_.globals) {
            if (global.decl == nullptr) continue;
            resolve_type_dependencies(global.decl->type);
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, global.decl->type);
            if (!layout.has_value()) {
                global.decl->resolved_alignment = 0;
                continue;
            }
            global.decl->resolved_alignment =
                engine_.resolve_root_alignment_specs(global.decl->alignment_specs, layout->abi_align_bytes, global.decl->loc,
                                                     "variable '" + global.decl->var_name + "'");
        }
        for (Function& fn : program_.functions) {
            if (!fn.body) continue;
            if (fn.is_generic_template) continue;
            if (!fn.member_owner_class.empty() && generic_template_owner_names_.contains(fn.member_owner_class)) {
                continue;
            }
            engine_.validate_constexpr_locals(fn);
        }
    }

private:
    Program& program_;
    ConstexprEngine& engine_;
    std::unordered_set<std::string> resolving_structs_;
    std::unordered_set<std::string> resolved_structs_;
    std::unordered_set<std::string> resolving_classes_;
    std::unordered_set<std::string> resolved_classes_;
    std::unordered_set<std::string> generic_template_owner_names_;
    // ch05 §9.4: separate tracking sets for `resolve_array_bounds()`'s own
    // early pass above -- deliberately NOT shared with
    // resolving_structs_/resolved_structs_ (which mean "fully resolved,
    // alignas included" for `run()`'s later, different pass). Reusing
    // those would make `run()` wrongly skip a struct/class's alignas work
    // entirely, believing it already fully resolved.
    std::unordered_set<std::string> array_bounds_resolving_structs_;
    std::unordered_set<std::string> array_bounds_resolved_structs_;
    std::unordered_set<std::string> array_bounds_resolving_classes_;
    std::unordered_set<std::string> array_bounds_resolved_classes_;

    // Named-type-aware recursive Type walker used only by
    // resolve_array_bounds() above -- mirrors resolve_type_dependencies
    // below but resolves ONLY array bounds (no alignas/layout work), and
    // additionally recurses into a Named struct/class reference so a
    // `sizeof(Other)` inside an array-bound expression sees Other's own
    // array-sized fields correctly, regardless of declaration order.
    void resolve_array_bounds_type_dependencies(Type& type) {
        switch (type.kind) {
            case TypeKind::Named:
                if (StructDef* def = find_struct_mut(type.name)) {
                    resolve_struct_array_bounds(def->name);
                } else if (ClassDef* def = find_class_mut(type.name)) {
                    resolve_class_array_bounds(def->name);
                }
                return;
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) resolve_array_bounds_type_dependencies(*type.pointee);
                return;
            case TypeKind::Array:
                if (type.element) resolve_array_bounds_type_dependencies(*type.element);
                if (type.array_size_expr) {
                    type.array_size = engine_.resolve_root_array_bound(*type.array_size_expr);
                    type.array_size_expr.reset();
                }
                return;
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) resolve_array_bounds_type_dependencies(*type.function_return);
                for (Type& param : type.function_params) resolve_array_bounds_type_dependencies(param);
                return;
        }
    }

    // Full statement-tree walk covering every StmtKind that can carry a
    // nested VarDecl or expression -- mirrors collect_runtime_stmt_rewrites/
    // collect_runtime_expr_rewrites's own traversal shape (the two
    // functions this pass exists specifically to run ahead of). Also
    // mirrors validate_constexpr_stmt_tree's own frame-scoping shape (see
    // ConstexprEngine::push_local_array_bound_scope's comment): a nested
    // frame per Block/If-branch/While-body keeps a local constexpr's
    // visibility scoped exactly like ordinary C++ block scoping.
    void resolve_array_bounds_in_stmt(Stmt& stmt) {
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
                engine_.resolve_array_bounds_in_type(stmt.type);
                if (stmt.init) resolve_array_bounds_in_expr(*stmt.init);
                for (ExprPtr& arg : stmt.ctor_args) resolve_array_bounds_in_expr(*arg);
                // Makes this local's own value visible to a later
                // sibling/nested array-bound expression in this same
                // function, exactly like it's already visible as an
                // `alignas` operand.
                engine_.bind_local_constant_for_array_bounds(stmt);
                return;
            case StmtKind::Return:
            case StmtKind::ExprStmt:
                if (stmt.expr) resolve_array_bounds_in_expr(*stmt.expr);
                return;
            case StmtKind::If:
                if (stmt.condition) resolve_array_bounds_in_expr(*stmt.condition);
                if (stmt.then_branch) {
                    engine_.push_local_array_bound_scope();
                    try {
                        resolve_array_bounds_in_stmt(*stmt.then_branch);
                    } catch (...) {
                        engine_.pop_local_array_bound_scope();
                        throw;
                    }
                    engine_.pop_local_array_bound_scope();
                }
                if (stmt.else_branch) {
                    engine_.push_local_array_bound_scope();
                    try {
                        resolve_array_bounds_in_stmt(*stmt.else_branch);
                    } catch (...) {
                        engine_.pop_local_array_bound_scope();
                        throw;
                    }
                    engine_.pop_local_array_bound_scope();
                }
                return;
            case StmtKind::While:
                if (stmt.condition) resolve_array_bounds_in_expr(*stmt.condition);
                if (stmt.then_branch) {
                    engine_.push_local_array_bound_scope();
                    try {
                        resolve_array_bounds_in_stmt(*stmt.then_branch);
                    } catch (...) {
                        engine_.pop_local_array_bound_scope();
                        throw;
                    }
                    engine_.pop_local_array_bound_scope();
                }
                return;
            case StmtKind::Block:
                engine_.push_local_array_bound_scope();
                try {
                    for (StmtPtr& nested : stmt.statements) resolve_array_bounds_in_stmt(*nested);
                } catch (...) {
                    engine_.pop_local_array_bound_scope();
                    throw;
                }
                engine_.pop_local_array_bound_scope();
                return;
            case StmtKind::Break:
            case StmtKind::Continue:
                return;
        }
    }

    void resolve_array_bounds_in_expr(Expr& expr) {
        if (expr.lhs) resolve_array_bounds_in_expr(*expr.lhs);
        if (expr.rhs) resolve_array_bounds_in_expr(*expr.rhs);
        if (expr.third) resolve_array_bounds_in_expr(*expr.third);
        for (ExprPtr& arg : expr.args) resolve_array_bounds_in_expr(*arg);
        if (expr.lambda_body) resolve_array_bounds_in_stmt(*expr.lambda_body);
    }

    // Array-bounds-only counterpart of resolve_struct/resolve_class below
    // (no alignas work, and using this class's own separate
    // array_bounds_resolving_*/array_bounds_resolved_* sets).
    void resolve_struct_array_bounds(const std::string& name) {
        if (array_bounds_resolved_structs_.contains(name)) return;
        if (!array_bounds_resolving_structs_.insert(name).second) return;
        StructDef* def = find_struct_mut(name);
        if (!def) return;
        if (!def->template_params.empty()) {
            array_bounds_resolving_structs_.erase(name);
            array_bounds_resolved_structs_.insert(name);
            return;
        }
        engine_.mark_type_incomplete(name);
        for (StructField& field : def->fields) resolve_array_bounds_type_dependencies(field.type);
        engine_.mark_type_complete(name);
        array_bounds_resolving_structs_.erase(name);
        array_bounds_resolved_structs_.insert(name);
    }

    void resolve_class_array_bounds(const std::string& name) {
        if (array_bounds_resolved_classes_.contains(name)) return;
        if (!array_bounds_resolving_classes_.insert(name).second) return;
        ClassDef* def = find_class_mut(name);
        if (!def) return;
        if (!def->template_params.empty()) {
            array_bounds_resolving_classes_.erase(name);
            array_bounds_resolved_classes_.insert(name);
            return;
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
        if (def->is_synthetic_check_only) {
            array_bounds_resolving_classes_.erase(name);
            array_bounds_resolved_classes_.insert(name);
            return;
        }
        engine_.mark_type_incomplete(name);
        if (BaseSpecifier* base = def->direct_ordinary_base()) resolve_array_bounds_type_dependencies(base->base_type);
        for (ClassField& field : def->fields) resolve_array_bounds_type_dependencies(field.type);
        engine_.mark_type_complete(name);
        array_bounds_resolving_classes_.erase(name);
        array_bounds_resolved_classes_.insert(name);
    }

    [[nodiscard]] StructDef* find_struct_mut(std::string_view name) {
        for (StructDef& def : program_.structs) {
            if (def.name == name) return &def;
        }
        return nullptr;
    }

    [[nodiscard]] ClassDef* find_class_mut(std::string_view name) {
        for (ClassDef& def : program_.classes) {
            if (def.name == name) return &def;
        }
        return nullptr;
    }

    void resolve_type_dependencies(Type& type) {
        switch (type.kind) {
            case TypeKind::Named:
                if (StructDef* def = find_struct_mut(type.name)) {
                    resolve_struct(def->name);
                } else if (ClassDef* def = find_class_mut(type.name)) {
                    resolve_class(def->name);
                }
                return;
            case TypeKind::Pointer:
            case TypeKind::Reference:
            case TypeKind::Span:
                if (type.pointee) resolve_type_dependencies(*type.pointee);
                return;
            case TypeKind::Array:
                if (type.element) resolve_type_dependencies(*type.element);
                // ch05 §9.4: this array's own bound (not its element's --
                // that was just handled by the recursive call above)
                // must be resolved before any layout_of_type call below
                // (natural_field_alignment/natural_struct_alignment/
                // natural_class_alignment, and this same function's own
                // callers) ever reads `array_size`.
                if (type.array_size_expr) {
                    type.array_size = engine_.resolve_root_array_bound(*type.array_size_expr);
                    type.array_size_expr.reset();
                }
                return;
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                if (type.function_return) resolve_type_dependencies(*type.function_return);
                for (Type& param : type.function_params) resolve_type_dependencies(param);
                return;
        }
    }

    [[nodiscard]] bool type_has_strengthened_record_alignment(const Type& type) {
        if (type.kind == TypeKind::Array && type.element) return type_has_strengthened_record_alignment(*type.element);
        if (type.kind != TypeKind::Named) return false;
        if (StructDef* def = find_struct_mut(type.name)) return def->resolved_alignment != 0;
        if (ClassDef* def = find_class_mut(type.name)) return def->resolved_alignment != 0;
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
            if (std::optional<std::uint64_t> align = natural_field_alignment(field)) {
                overall = std::max(overall, *align);
            }
        }
        return overall;
    }

    [[nodiscard]] std::uint64_t natural_class_alignment(const ClassDef& def) {
        std::uint64_t overall = 1;
        if (const BaseSpecifier* base = def.direct_ordinary_base()) {
            std::optional<TypeLayoutInfo> base_layout = layout_of_type(program_, base->base_type);
            if (base_layout.has_value()) overall = std::max(overall, base_layout->abi_align_bytes);
        }
        for (const ClassField& field : def.fields) {
            if (std::optional<std::uint64_t> align = natural_field_alignment(field)) {
                overall = std::max(overall, *align);
            }
        }
        return overall;
    }

    void resolve_struct(const std::string& name) {
        if (resolved_structs_.contains(name)) return;
        if (!resolving_structs_.insert(name).second) return;
        StructDef* def = find_struct_mut(name);
        if (!def) return;
        if (!def->template_params.empty()) {
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
            return;
        }
        engine_.mark_type_incomplete(name);
        for (StructField& field : def->fields) {
            resolve_type_dependencies(field.type);
        }
        if (def->is_packed && !def->alignment_specs.empty()) {
            throw ConstexprError(def->alignment_specs.front().loc,
                                 "'[[scpp::packed]]' cannot be combined with 'alignas' on '" + def->name + "'");
        }
        for (StructField& field : def->fields) {
            if (def->is_packed && !field.alignment_specs.empty()) {
                throw ConstexprError(field.alignment_specs.front().loc,
                                     "'[[scpp::packed]]' cannot be combined with 'alignas' on member '" + field.name + "'");
            }
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
            if (!layout.has_value()) {
                field.resolved_alignment = 0;
                continue;
            }
            field.resolved_alignment =
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, field.loc,
                                                     "member '" + field.name + "'");
            if (def->is_packed && type_has_strengthened_record_alignment(field.type)) {
                throw ConstexprError(field.loc,
                                     "'[[scpp::packed]]' member '" + field.name +
                                         "' cannot have a class/struct/union type whose alignment was strengthened by "
                                         "'alignas'");
            }
        }
        std::uint64_t natural_align = natural_struct_alignment(*def);
        def->resolved_alignment =
            engine_.resolve_root_alignment_specs(def->alignment_specs, natural_align, def->loc,
                                                 std::string(def->is_union ? "union '" : "struct '") + def->name + "'");
        engine_.mark_type_complete(name);
        resolving_structs_.erase(name);
        resolved_structs_.insert(name);
    }

    void resolve_class(const std::string& name) {
        if (resolved_classes_.contains(name)) return;
        if (!resolving_classes_.insert(name).second) return;
        ClassDef* def = find_class_mut(name);
        if (!def) return;
        if (!def->template_params.empty()) {
            // See the identical comment in resolve_struct above.
            resolving_classes_.erase(name);
            resolved_classes_.insert(name);
            return;
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
        if (def->is_synthetic_check_only) {
            resolving_classes_.erase(name);
            resolved_classes_.insert(name);
            return;
        }
        engine_.mark_type_incomplete(name);
        if (BaseSpecifier* base = def->direct_ordinary_base()) resolve_type_dependencies(base->base_type);
        for (ClassField& field : def->fields) resolve_type_dependencies(field.type);
        for (ClassField& field : def->fields) {
            std::optional<TypeLayoutInfo> layout = layout_of_type(program_, field.type);
            if (!layout.has_value()) {
                field.resolved_alignment = 0;
                continue;
            }
            field.resolved_alignment =
                engine_.resolve_root_alignment_specs(field.alignment_specs, layout->abi_align_bytes, field.loc,
                                                     "member '" + field.name + "'");
        }
        std::uint64_t natural_align = natural_class_alignment(*def);
        def->resolved_alignment =
            engine_.resolve_root_alignment_specs(def->alignment_specs, natural_align, def->loc, "class '" + def->name + "'");
        engine_.mark_type_complete(name);
        resolving_classes_.erase(name);
        resolved_classes_.insert(name);
    }
};

} // namespace

void fold_immediate_calls(Program& program, ConstexprLimits limits) {
    ConstexprEngine engine(program, limits);
    AlignmentResolver aligner(program, engine);
    // ch05 §9.4: array bounds must be resolved before *any* immediate-call
    // folding below, since folding a call to a consteval function actually
    // executes its body (via evaluate_root_expr/execute_stmt) -- including
    // any local array declaration inside it -- long before `aligner.run()`
    // would otherwise get around to validating that same function. See
    // resolve_array_bounds()'s own comment for the full rationale.
    aligner.resolve_array_bounds();
    std::vector<ExprRewrite> expr_rewrites;
    std::vector<Stmt*> consteval_if_rewrites;
    for (Function& fn : program.functions) {
        if (!fn.body) continue;
        collect_runtime_stmt_rewrites(program, *fn.body, engine, expr_rewrites, consteval_if_rewrites);
    }
    for (ExprRewrite& rewrite : expr_rewrites) rewrite_expr_as_constant(*rewrite.target, rewrite.value);
    // Validate required-constant-expression contexts *before* stripping
    // `if consteval` / `if !consteval` down to their runtime-selected
    // branches. Otherwise a `constexpr` local initializer that calls a
    // constexpr function containing `if consteval` would incorrectly see
    // only the runtime branch, because the callee's AST has already been
    // destructively rewritten for the later runtime pipeline.
    aligner.run();
    for (Stmt* stmt : consteval_if_rewrites) rewrite_consteval_if_for_runtime(*stmt);
    for (Function& fn : program.functions) {
        if (fn.eval_mode == FunctionEvalMode::Consteval && !fn.name.ends_with("_new")) fn.body.reset();
    }
}

ConstexprValue evaluate_immediate_expr(const Program& program, const Expr& expr, ConstexprLimits limits) {
    ConstexprEngine engine(program, limits);
    return snapshot_constexpr_value(engine.evaluate_root_expr(expr), expr.loc);
}

} // namespace scpp
