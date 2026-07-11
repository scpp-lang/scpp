module;

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module scpp.constexpr_engine;

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
            for (size_t i = 0; i < a.template_args.size(); ++i) {
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
            for (size_t i = 0; i < a.function_params.size(); ++i) {
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

[[nodiscard]] bool is_integer_like(const Type& type) {
    return is_named_type(type, "int") || is_named_type(type, "char") || is_named_type(type, "bool");
}

[[nodiscard]] bool is_floating_like(const Type& type) { return is_named_type(type, "double"); }

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
    if (pointer.index < 0 || static_cast<size_t>(pointer.index) >= array->elements.size()) {
        throw ConstexprError(loc, "constexpr dereference out of bounds");
    }
    return array->elements[static_cast<size_t>(pointer.index)];
}

class ConstexprEngine {
public:
    ConstexprEngine(const Program& program, ConstexprLimits limits)
        : program_(program), limits_(limits) {
        for (size_t i = 0; i < program_.functions.size(); ++i) functions_by_name_[program_.functions[i].name].push_back(i);
        for (const ClassDef& def : program_.classes) classes_by_name_.emplace(def.name, &def);
        for (const StructDef& def : program_.structs) structs_by_name_.emplace(def.name, &def);
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

private:
    const Program& program_;
    ConstexprLimits limits_;
    int steps_ = 0;
    int call_depth_ = 0;
    int string_storage_counter_ = 0;
    std::vector<std::unordered_map<std::string, Binding>> frames_;
    std::unordered_map<std::string, std::vector<size_t>> functions_by_name_;
    std::unordered_map<std::string, const ClassDef*> classes_by_name_;
    std::unordered_map<std::string, const StructDef*> structs_by_name_;

    void tick(const SourceLocation& loc, std::string_view what) {
        ++steps_;
        if (steps_ > limits_.max_steps) {
            throw ConstexprError(loc, "constexpr evaluation exceeded step budget while " + std::string(what));
        }
    }

    void validate_constexpr_stmt_tree(Stmt& stmt) {
        tick(stmt.loc, "checking a constexpr local declaration");
        switch (stmt.kind) {
            case StmtKind::VarDecl:
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
        if (!def.base_class_name.empty()) {
            auto base_it = classes_by_name_.find(def.base_class_name);
            if (base_it == classes_by_name_.end()) {
                throw ConstexprError({}, "missing constexpr class definition for base class '" + def.base_class_name + "'");
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
                if (type.name == "int" || type.name == "char") {
                    cell->data = 0LL;
                    return cell;
                }
                if (type.name == "bool") {
                    cell->data = false;
                    return cell;
                }
                if (type.name == "double") {
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
                        object.fields.emplace(field.name, make_default_cell(field.type, loc));
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
        throw ConstexprError(loc, "expression is not a constant expression: identifier '" + name + "' is not available");
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

    void checked_assign_integer(const std::shared_ptr<Cell>& target, long long value, const SourceLocation& loc) {
        if (is_named_type(target->type, "char")) {
            if (value < 0 || value > 255) throw ConstexprError(loc, "constexpr char conversion overflow");
            target->data = value;
            return;
        }
        if (is_named_type(target->type, "bool")) {
            target->data = (value != 0);
            return;
        }
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw ConstexprError(loc, "constexpr integer overflow");
        }
        target->data = value;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_checked_int_cell(long long value, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("int");
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
                    if (index < 0 || static_cast<size_t>(index) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr subscript out of bounds");
                    }
                    return LValue{array->elements[static_cast<size_t>(index)], base_read_only};
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
                    if (shifted.index < 0 || static_cast<size_t>(shifted.index) >= array->elements.size()) {
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
        for (size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (size_t i = 0; i < args.size(); ++i) {
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
        for (size_t fn_index : it->second) {
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
            if (it == classes_by_name_.end() || it->second->base_class_name.empty()) return false;
            current = it->second->base_class_name;
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
        for (size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != args.size() + 1) continue;
            bool params_match = true;
            for (size_t i = 0; i < args.size(); ++i) {
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
        for (size_t fn_index : it->second) {
            const Function* fn = &program_.functions[fn_index];
            if (!fn->body || fn->eval_mode != FunctionEvalMode::RuntimeOnly || fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (size_t i = 0; i < args.size(); ++i) {
                if (!constexpr_argument_matches_parameter(fn->params[i].type, args[i], /*require_constexpr=*/false)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return true;
        }
        return false;
    }

    [[nodiscard]] bool is_class_name(std::string_view name) const {
        return classes_by_name_.contains(std::string(name));
    }

    [[nodiscard]] bool has_user_defined_destructor(std::string_view class_name) const {
        auto it = functions_by_name_.find(std::string(class_name) + "_delete");
        if (it == functions_by_name_.end()) return false;
        for (size_t fn_index : it->second) {
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
            switch (expr.binary_op) {
                case BinaryOp::Add: {
                    long long result;
                    if (__builtin_add_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Sub: {
                    long long result;
                    if (__builtin_sub_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Mul: {
                    long long result;
                    if (__builtin_mul_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Div:
                    if (right == 0) throw ConstexprError(expr.loc, "constexpr division by zero");
                    return make_checked_int_cell(left / right, expr.loc);
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
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (fn.params[i].type.kind == TypeKind::Reference) continue;
            reject_user_defined_destructor_execution(fn.params[i].type, loc);
        }
        frames_.push_back({});
        auto& frame = frames_.back();
        for (size_t i = 0; i < fn.params.size(); ++i) frame.emplace(fn.params[i].name, std::move(bindings[i]));
        try {
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
        for (size_t i = 0; i < fn.params.size(); ++i) {
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
        for (size_t fn_index : it->second) {
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
            for (size_t i = 0; i < arg_values.size(); ++i) {
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
            if (has_runtime_only_match(expr.name + "_new", arg_values)) {
                throw ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval constructors");
            }
            throw ConstexprError(expr.loc, "no constexpr/consteval constructor matches for type '" + expr.name + "'");
        }
        std::vector<Binding> bindings;
        bindings.reserve(ctor->params.size());
        bindings.push_back(Binding{object, false});
        for (size_t i = 1; i < ctor->params.size(); ++i) {
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

    [[nodiscard]] std::shared_ptr<Cell> evaluate_expr(const Expr& expr) {
        tick(expr.loc, "evaluating an expression");
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return make_scalar_cell(named_type("int"), expr.int_value);
            case ExprKind::FloatLiteral: return make_double_cell(expr.float_value);
            case ExprKind::BoolLiteral: return make_bool_cell(expr.bool_value);
            case ExprKind::CharLiteral: return make_scalar_cell(named_type("char"), expr.int_value);
            case ExprKind::StringLiteral: return make_string_literal_pointer(expr);
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
            case ExprKind::Call: return evaluate_call_expr(expr);
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
                        throw ConstexprError(stmt.loc, "no constexpr/consteval constructor matches for type '" + stmt.type.name + "'");
                    }
                    for (size_t i = 1; i < ctor->params.size(); ++i) {
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
        for (size_t i = static_cast<size_t>(std::max(pointer->index, 0LL)); i < array->elements.size(); ++i) {
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

} // namespace

void fold_immediate_calls(Program& program, ConstexprLimits limits) {
    ConstexprEngine engine(program, limits);
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
    for (Function& fn : program.functions) {
        if (!fn.body) continue;
        engine.validate_constexpr_locals(fn);
    }
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
